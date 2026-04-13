// Package device manages the serial connection to the ESP32 and provides
// a request/response API matched by sequence number, plus unsolicited
// push-frame dispatch for TCP tunnelling.
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
	tcpMaxConns    = 16
	// initialCredit must match TCP_SNDBUF on the ESP32 side (tcp_mgr.c).
	// Seeds the credit window so ctrl can immediately fill the socket send
	// buffer without waiting for the first CMD_TCP_SEND_CREDIT push.
	initialCredit  = 5760
)

var (
	ErrNotConnected = errors.New("device not connected")
	ErrTimeout      = errors.New("response timeout")
	ErrNoSlots      = errors.New("no free TCP connection slots on device")
)

// TCPConn is a TCP connection tunnelled via the ESP32's WiFi.
// Use Read to receive data pushed by the device and Write to send data.
type TCPConn struct {
	id     byte
	dev    *Device
	recvCh chan []byte // closed via shutdownOnce
	// shutdownOnce closes recvCh and wakes blocked writers; used by
	// both the explicit Close() path and the device-push path.
	shutdownOnce sync.Once
	// cmdCloseOnce guards the single CMD_TCP_CLOSE transmission.
	cmdCloseOnce sync.Once

	// Credit flow control (T11)
	creditMu   sync.Mutex
	creditCond *sync.Cond
	credit     int
	closed     bool
}

// shutdown tears down the local side of the connection: closes recvCh
// so Read returns io.EOF, and wakes any Write blocked on credit.
// Safe to call multiple times.
func (c *TCPConn) shutdown() {
	c.shutdownOnce.Do(func() {
		c.creditMu.Lock()
		c.closed = true
		c.creditCond.Broadcast()
		c.creditMu.Unlock()
		close(c.recvCh)
	})
}

// Read blocks until the device pushes data for this connection,
// or returns io.EOF when the connection is closed.
func (c *TCPConn) Read() ([]byte, error) {
	data, ok := <-c.recvCh
	if !ok {
		return nil, io.EOF
	}
	return data, nil
}

// Write sends data through the ESP32 to the remote TCP peer.
// Large payloads are automatically split into protocol-sized chunks.
// Each chunk waits for sufficient send credit before transmission.
func (c *TCPConn) Write(data []byte) error {
	const maxChunk = proto.MaxPayload - 1 // 1 byte reserved for conn_id
	for len(data) > 0 {
		n := len(data)
		if n > maxChunk {
			n = maxChunk
		}

		// Wait until we have credit for this chunk (T11).
		c.creditMu.Lock()
		for c.credit < n && !c.closed {
			c.creditCond.Wait()
		}
		if c.closed {
			c.creditMu.Unlock()
			return errors.New("connection closed")
		}
		c.credit -= n
		c.creditMu.Unlock()

		payload := make([]byte, 1+n)
		payload[0] = c.id
		copy(payload[1:], data[:n])
		if err := c.dev.writeFrameNoReply(proto.CmdTCPSend, payload); err != nil {
			return err
		}
		data = data[n:]
	}
	return nil
}

// Close tears down the TCP connection on the device side.
// Idempotent; safe to call from multiple goroutines.
func (c *TCPConn) Close() error {
	// Unblock any blocked Read/Write immediately.
	c.shutdown()
	c.dev.unregisterTCPConn(c.id)

	// Send CMD_TCP_CLOSE exactly once.
	var err error
	c.cmdCloseOnce.Do(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_, err = c.dev.Send(ctx, proto.CmdTCPClose, []byte{c.id})
	})
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

	// TCP push dispatch
	tcpMu    sync.Mutex
	tcpConns map[byte]*TCPConn

	// Pending async TCP connect channels (T9)
	connectMu      sync.Mutex
	pendingConnect map[byte]chan error

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// New creates a Device but does not open the port yet.
func New(portName string, baudRate int) *Device {
	ctx, cancel := context.WithCancel(context.Background())
	return &Device{
		portName:       portName,
		baudRate:       baudRate,
		pending:        make(map[byte]chan proto.Frame),
		tcpConns:       make(map[byte]*TCPConn),
		pendingConnect: make(map[byte]chan error),
		ctx:            ctx,
		cancel:         cancel,
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

	// Unblock any goroutine waiting for a TCP connect result.
	d.connectMu.Lock()
	for id, ch := range d.pendingConnect {
		ch <- ErrNotConnected
		delete(d.pendingConnect, id)
	}
	d.connectMu.Unlock()

	// Close all open TCP connections and wake blocked writers.
	d.tcpMu.Lock()
	for id, conn := range d.tcpConns {
		conn.shutdown()
		delete(d.tcpConns, id)
	}
	d.tcpMu.Unlock()
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
// Used for fire-and-forget commands like CmdTCPSend.
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

// TCPConnect opens a TCP connection via the ESP32 and returns a TCPConn.
// The connection attempt happens asynchronously on the device; this function
// waits for CMD_TCP_CONNECT_DONE before returning (T9).
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

	// Step 1: Send CMD_TCP_CONNECT — device allocates a slot and responds
	// with the conn_id immediately (async connect queued on device).
	f, err := d.Send(ctx, proto.CmdTCPConnect, payload)
	if err != nil {
		return nil, fmt.Errorf("tcp connect %s:%d: %w", host, port, err)
	}
	if f.Status != proto.StatusOK {
		return nil, fmt.Errorf("tcp connect %s:%d: device status %s", host, port, f.Status)
	}
	if len(f.Payload) < 1 {
		return nil, errors.New("tcp connect: short response")
	}
	connID := f.Payload[0]

	// Step 2: Register a channel to receive the async connect result.
	doneCh := make(chan error, 1)
	d.connectMu.Lock()
	d.pendingConnect[connID] = doneCh
	d.connectMu.Unlock()
	defer func() {
		d.connectMu.Lock()
		delete(d.pendingConnect, connID)
		d.connectMu.Unlock()
	}()

	// Step 3: Wait for CMD_TCP_CONNECT_DONE push.
	select {
	case connectErr := <-doneCh:
		if connectErr != nil {
			return nil, fmt.Errorf("tcp connect %s:%d: %w", host, port, connectErr)
		}
	case <-ctx.Done():
		// Timed out — tell ESP32 to free the slot so it isn't occupied for 60 s.
		closeCtx, closeCancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer closeCancel()
		d.Send(closeCtx, proto.CmdTCPClose, []byte{connID}) //nolint:errcheck
		return nil, fmt.Errorf("tcp connect %s:%d: timeout waiting for connect done", host, port)
	case <-d.ctx.Done():
		return nil, ErrNotConnected
	}

	// Step 4: Connection succeeded — create TCPConn with seed credit (T11).
	conn := &TCPConn{
		id:     connID,
		dev:    d,
		recvCh: make(chan []byte, 64),
		credit: initialCredit,
	}
	conn.creditCond = sync.NewCond(&conn.creditMu)

	d.tcpMu.Lock()
	d.tcpConns[connID] = conn
	d.tcpMu.Unlock()

	// Step 5: Send ACK so ESP32's rx task starts delivering data.
	if err := d.writeFrameNoReply(proto.CmdTcpConnectAck, []byte{connID}); err != nil {
		d.unregisterTCPConn(connID)
		return nil, fmt.Errorf("ack tcp connect error: %w", err)
	}
	slog.Debug("tcp connected", "id", connID, "host", host, "port", port)
	return conn, nil
}

func (d *Device) unregisterTCPConn(id byte) {
	d.tcpMu.Lock()
	delete(d.tcpConns, id)
	d.tcpMu.Unlock()
}

// dispatchPush handles unsolicited push frames from the device.
func (d *Device) dispatchPush(f proto.Frame) {
	switch f.Cmd {
	case proto.CmdTCPDataPush:
		if len(f.Payload) < 1 {
			return
		}
		connID := f.Payload[0]
		data := f.Payload[1:]
		d.tcpMu.Lock()
		conn := d.tcpConns[connID]
		d.tcpMu.Unlock()
		if conn != nil && len(data) > 0 {
			select {
			case conn.recvCh <- append([]byte(nil), data...):
			default:
				slog.Warn("tcp recv buffer full, dropping data", "conn", connID)
			}
		}

	case proto.CmdTCPClosedPush:
		if len(f.Payload) < 1 {
			return
		}
		connID := f.Payload[0]
		d.tcpMu.Lock()
		conn := d.tcpConns[connID]
		delete(d.tcpConns, connID)
		d.tcpMu.Unlock()
		if conn != nil {
			conn.shutdown()
			slog.Debug("tcp conn closed by device", "conn", connID)
		}

	case proto.CmdTCPConnectDone: // T9: async connect result
		if len(f.Payload) < 2 {
			return
		}
		connID, status := f.Payload[0], f.Payload[1]
		d.connectMu.Lock()
		ch := d.pendingConnect[connID]
		delete(d.pendingConnect, connID)
		d.connectMu.Unlock()
		if ch != nil {
			if status == 0 {
				ch <- nil
			} else {
				ch <- fmt.Errorf("connect failed: errno %d", status)
			}
		}

	case proto.CmdTCPSendCredit: // T11: replenish send credit
		if len(f.Payload) < 3 {
			return
		}
		connID := f.Payload[0]
		credits := int(f.Payload[1])<<8 | int(f.Payload[2])
		d.tcpMu.Lock()
		conn := d.tcpConns[connID]
		d.tcpMu.Unlock()
		if conn != nil {
			conn.creditMu.Lock()
			conn.credit += credits
			conn.creditCond.Broadcast()
			conn.creditMu.Unlock()
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
