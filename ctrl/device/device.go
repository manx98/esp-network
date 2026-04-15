// Package device manages the serial connection to the ESP32 and provides
// a request/response API matched by sequence number, plus unsolicited
// push-frame dispatch for the single proxy relay connection.
package device

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"sync"
	"sync/atomic"
	"time"

	"go.bug.st/serial"

	"github.com/manx98/esp32-ctrl/proto"
)

const (
	defaultBaud    = 115200
	readBufSize    = 512
	defaultTimeout = 15 * time.Second
	tcpConnTimeout = 15 * time.Second
)

var (
	ErrNotConnected = errors.New("device not connected")
	ErrTimeout      = errors.New("response timeout")
)

// TCPConn is the single relay connection tunnelled through the ESP32 to the
// proxy server.  Write sends raw bytes via CMD_PROXY_SEND; Read returns raw
// bytes delivered by CMD_PROXY_DATA_PUSH push frames.
type TCPConn struct {
	dev       *Device
	recvCh    chan []byte
	closeOnce sync.Once
}

// shutdown closes recvCh so any blocked Read returns io.EOF.
// Idempotent; safe to call from multiple goroutines.
func (c *TCPConn) shutdown() {
	c.closeOnce.Do(func() {
		close(c.recvCh)
	})
}

// Read blocks until the proxy server sends data, or returns io.EOF when
// the relay connection is closed.
func (c *TCPConn) Read() ([]byte, error) {
	data, ok := <-c.recvCh
	if !ok {
		return nil, io.EOF
	}
	return data, nil
}

// Write sends raw bytes to the proxy server through the ESP32 relay.
// Large payloads are split into protocol-sized chunks automatically.
func (c *TCPConn) Write(data []byte) error {
	const maxChunk = proto.MaxPayload
	for len(data) > 0 {
		n := len(data)
		if n > maxChunk {
			n = maxChunk
		}
		if err := c.dev.writeFrameNoReply(proto.CmdProxySend, data[:n]); err != nil {
			return err
		}
		data = data[n:]
	}
	return nil
}

// Close tears down the relay connection on both sides.
// Idempotent; safe to call from multiple goroutines.
func (c *TCPConn) Close() error {
	c.shutdown()
	c.dev.clearRelayConn(c)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err := c.dev.Send(ctx, proto.CmdProxyDisconnect, nil)
	return err
}

// Device represents a connected ESP32 over USB-CDC serial.
type Device struct {
	portName string
	baudRate int

	mu      sync.Mutex
	port    serial.Port
	writeMu sync.Mutex // serialises all serial writes

	seqCounter atomic.Uint32

	pendingMu sync.Mutex
	pending   map[byte]chan proto.Frame

	// Single relay connection to the proxy server.
	relayMu   sync.Mutex
	relayConn *TCPConn

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// New creates a Device but does not open the port yet.
func New(portName string, baudRate int) *Device {
	ctx, cancel := context.WithCancel(context.Background())
	return &Device{
		portName: portName,
		baudRate: baudRate,
		pending:  make(map[byte]chan proto.Frame),
		ctx:      ctx,
		cancel:   cancel,
	}
}

// Open opens the serial port and starts the background reader.
func (d *Device) Open() error {
	d.mu.Lock()
	defer d.mu.Unlock()

	mode := &serial.Mode{
		BaudRate: d.baudRate,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}
	port, err := serial.Open(d.portName, mode)
	if err != nil {
		return fmt.Errorf("open %s: %w", d.portName, err)
	}
	d.port = port

	d.wg.Add(1)
	go d.readLoop()

	slog.Info("device opened", "port", d.portName, "baud", d.baudRate)
	return nil
}

// Close shuts down the reader and closes the port.
func (d *Device) Close() {
	d.cancel()

	d.mu.Lock()
	if d.port != nil {
		_ = d.port.Close()
		d.port = nil
	}
	d.mu.Unlock()

	d.wg.Wait()

	// Drain all pending request waiters.
	d.pendingMu.Lock()
	for seq, ch := range d.pending {
		close(ch)
		delete(d.pending, seq)
	}
	d.pendingMu.Unlock()

	// Wake any blocked relay Read.
	d.relayMu.Lock()
	if d.relayConn != nil {
		d.relayConn.shutdown()
		d.relayConn = nil
	}
	d.relayMu.Unlock()
}

// IsConnected reports whether the serial port is open.
func (d *Device) IsConnected() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.port != nil
}

// Send transmits a request and waits for the matching response.
// The context deadline (or defaultTimeout if none) bounds the wait.
func (d *Device) Send(ctx context.Context, cmd proto.Cmd, payload []byte) (proto.Frame, error) {
	d.mu.Lock()
	port := d.port
	d.mu.Unlock()
	if port == nil {
		return proto.Frame{}, ErrNotConnected
	}

	seq := byte(d.seqCounter.Add(1) & 0xFF)

	ch := make(chan proto.Frame, 1)
	d.pendingMu.Lock()
	d.pending[seq] = ch
	d.pendingMu.Unlock()

	defer func() {
		d.pendingMu.Lock()
		delete(d.pending, seq)
		d.pendingMu.Unlock()
	}()

	raw := proto.BuildRequest(seq, cmd, payload)
	d.writeMu.Lock()
	_, err := port.Write(raw)
	d.writeMu.Unlock()
	if err != nil {
		return proto.Frame{}, fmt.Errorf("write: %w", err)
	}

	timeout := defaultTimeout
	if dl, ok := ctx.Deadline(); ok {
		if rem := time.Until(dl); rem < timeout {
			timeout = rem
		}
	}

	select {
	case f, ok := <-ch:
		if !ok {
			return proto.Frame{}, ErrNotConnected
		}
		return f, nil
	case <-time.After(timeout):
		return proto.Frame{}, ErrTimeout
	case <-ctx.Done():
		return proto.Frame{}, ctx.Err()
	case <-d.ctx.Done():
		return proto.Frame{}, ErrNotConnected
	}
}

// writeFrameNoReply sends a frame without waiting for a response (seq=0).
// Used for fire-and-forget commands like CmdProxySend.
func (d *Device) writeFrameNoReply(cmd proto.Cmd, payload []byte) error {
	d.mu.Lock()
	port := d.port
	d.mu.Unlock()
	if port == nil {
		return ErrNotConnected
	}
	raw := proto.BuildRequest(0, cmd, payload)
	d.writeMu.Lock()
	_, err := port.Write(raw)
	d.writeMu.Unlock()
	return err
}

// clearRelayConn removes conn from the relay slot if it is still current.
func (d *Device) clearRelayConn(conn *TCPConn) {
	d.relayMu.Lock()
	if d.relayConn == conn {
		d.relayConn = nil
	}
	d.relayMu.Unlock()
}

// TCPConnect establishes the single relay connection to the proxy server.
//
// This sends CMD_PROXY_CONNECT to the ESP32, which synchronously connects
// to host:port and returns OK or ERROR.  If the ESP32 reports the relay is
// already open (StatusBusy) the stale connection is torn down first and the
// connect is retried once.
func (d *Device) TCPConnect(host string, port uint16) (*TCPConn, error) {
	hostBytes := []byte(host)
	if len(hostBytes) > 253 {
		return nil, errors.New("hostname too long")
	}

	payload := make([]byte, 1+len(hostBytes)+2)
	payload[0] = byte(len(hostBytes))
	copy(payload[1:], hostBytes)
	payload[1+len(hostBytes)] = byte(port >> 8)
	payload[2+len(hostBytes)] = byte(port)

	ctx, cancel := context.WithTimeout(context.Background(), tcpConnTimeout)
	defer cancel()

	f, err := d.Send(ctx, proto.CmdProxyConnect, payload)
	if err != nil {
		return nil, fmt.Errorf("proxy connect %s:%d: %w", host, port, err)
	}

	// If the relay is stale (device still has the previous connection open),
	// force-disconnect on the device side and retry once.
	if f.Status == proto.StatusBusy {
		slog.Info("proxy: stale relay on device — disconnecting before retry")
		d.Send(ctx, proto.CmdProxyDisconnect, nil) //nolint:errcheck

		f, err = d.Send(ctx, proto.CmdProxyConnect, payload)
		if err != nil {
			return nil, fmt.Errorf("proxy connect %s:%d (retry): %w", host, port, err)
		}
	}

	if f.Status != proto.StatusOK {
		return nil, fmt.Errorf("proxy connect %s:%d: device status %s", host, port, f.Status)
	}

	conn := &TCPConn{
		dev:    d,
		recvCh: make(chan []byte, 64),
	}

	d.relayMu.Lock()
	// Evict any previous relay conn that was not properly cleaned up.
	if d.relayConn != nil {
		d.relayConn.shutdown()
	}
	d.relayConn = conn
	d.relayMu.Unlock()

	slog.Debug("proxy relay connected", "host", host, "port", port)
	return conn, nil
}

// dispatchPush handles unsolicited push frames from the device.
func (d *Device) dispatchPush(f proto.Frame) {
	switch f.Cmd {
	case proto.CmdProxyDataPush:
		d.relayMu.Lock()
		conn := d.relayConn
		d.relayMu.Unlock()
		if conn != nil && len(f.Payload) > 0 {
			select {
			case conn.recvCh <- append([]byte(nil), f.Payload...):
			default:
				slog.Warn("proxy recv buffer full, dropping data")
			}
		}

	case proto.CmdProxyClosedPush:
		d.relayMu.Lock()
		conn := d.relayConn
		d.relayConn = nil
		d.relayMu.Unlock()
		if conn != nil {
			conn.shutdown()
			slog.Debug("proxy relay closed by device")
		}
	}
}

// readLoop runs in its own goroutine, feeding serial bytes into the parser
// and dispatching completed frames.
func (d *Device) readLoop() {
	defer d.wg.Done()

	parser := proto.NewParser(func(f proto.Frame) {
		if f.IsResp {
			// Response to a pending request — match by seq.
			d.pendingMu.Lock()
			ch, ok := d.pending[f.Seq]
			if ok {
				delete(d.pending, f.Seq)
			}
			d.pendingMu.Unlock()
			if ok {
				select {
				case ch <- f:
				default:
				}
			}
		} else {
			// Unsolicited push from the device.
			d.dispatchPush(f)
		}
	})

	buf := make([]byte, readBufSize)
	for {
		select {
		case <-d.ctx.Done():
			return
		default:
		}

		d.mu.Lock()
		port := d.port
		d.mu.Unlock()
		if port == nil {
			return
		}

		n, err := port.Read(buf)
		if err != nil {
			select {
			case <-d.ctx.Done():
				return
			default:
				slog.Warn("serial read error", "err", err)
				d.mu.Lock()
				d.port = nil
				d.mu.Unlock()
				return
			}
		}
		if n > 0 {
			parser.Feed(buf[:n])
		}
	}
}
