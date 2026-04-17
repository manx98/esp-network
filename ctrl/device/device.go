// Package device manages the USB HID connection to the ESP32 and provides
// a request/response API matched by sequence number, plus unsolicited
// push-frame dispatch for the single proxy relay connection.
//
// USB HID report format (64 bytes each direction):
//
//	report[0]    = number of valid proto bytes in this report (0–63)
//	report[1..63]= proto bytes, zero-padded
//
// On Linux, /dev/hidrawN access requires either running as root or a udev rule:
//
//	SUBSYSTEM=="hidraw", ATTRS{idVendor}=="22fb", ATTRS{idProduct}=="1011", \
//	  MODE="0666"
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

	hid "github.com/sstallion/go-hid"

	"github.com/manx98/esp32-ctrl/proto"
)

const (
	// DefaultVID / DefaultPID match the user's ESP32 HID device.
	DefaultVID uint16 = 0x22fb
	DefaultPID uint16 = 0x1011

	hidReportSize  = 64  // bytes per USB HID report
	hidPayloadSize = 63  // usable bytes per report (byte 0 = length)
	readTimeout    = 100 // ms for ReadWithTimeout — allows ctx cancellation polling

	defaultTimeout = 15 * time.Second
	tcpConnTimeout = 15 * time.Second
	reconnectDelay = 3 * time.Second
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

func (c *TCPConn) shutdown() {
	c.closeOnce.Do(func() { close(c.recvCh) })
}

func (c *TCPConn) Read() ([]byte, error) {
	data, ok := <-c.recvCh
	if !ok {
		return nil, io.EOF
	}
	return data, nil
}

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

func (c *TCPConn) Close() error {
	c.shutdown()
	c.dev.clearRelayConn(c)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, err := c.dev.Send(ctx, proto.CmdProxyDisconnect, nil)
	return err
}

// Device represents a connected ESP32 over USB HID.
type Device struct {
	vid uint16
	pid uint16

	mu      sync.Mutex
	port    *hid.Device
	writeMu sync.Mutex // serialises all HID writes

	seqCounter atomic.Uint32

	pendingMu sync.Mutex
	pending   map[byte]chan proto.Frame

	relayMu   sync.Mutex
	relayConn *TCPConn

	ctx    context.Context
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

// New creates a Device but does not open the HID connection yet.
// Call Open() to start the auto-reconnect loop.
func New(vid, pid uint16) *Device {
	ctx, cancel := context.WithCancel(context.Background())
	return &Device{
		vid:     vid,
		pid:     pid,
		pending: make(map[byte]chan proto.Frame),
		ctx:     ctx,
		cancel:  cancel,
	}
}

// Open initialises hidapi and starts the background manage goroutine that
// keeps the HID connection alive, reconnecting automatically after failures.
// It always returns nil; connection errors are logged and retried.
func (d *Device) Open() error {
	if err := hid.Init(); err != nil {
		return fmt.Errorf("hid init: %w", err)
	}
	d.wg.Add(1)
	go d.manage()
	return nil
}

// Close shuts down the manage loop and closes the HID device.
func (d *Device) Close() {
	d.cancel()
	d.wg.Wait()

	d.mu.Lock()
	if d.port != nil {
		d.port.Close() //nolint:errcheck
		d.port = nil
	}
	d.mu.Unlock()

	hid.Exit() //nolint:errcheck

	d.pendingMu.Lock()
	for seq, ch := range d.pending {
		close(ch)
		delete(d.pending, seq)
	}
	d.pendingMu.Unlock()

	d.relayMu.Lock()
	if d.relayConn != nil {
		d.relayConn.shutdown()
		d.relayConn = nil
	}
	d.relayMu.Unlock()
}

// IsConnected reports whether the HID device is open.
func (d *Device) IsConnected() bool {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.port != nil
}

// Send transmits a request and waits for the matching response.
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
	if err := d.writeRaw(raw); err != nil {
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
func (d *Device) writeFrameNoReply(cmd proto.Cmd, payload []byte) error {
	d.mu.Lock()
	port := d.port
	d.mu.Unlock()
	if port == nil {
		return ErrNotConnected
	}
	return d.writeRaw(proto.BuildRequest(0, cmd, payload))
}

// writeRaw splits raw bytes into hidPayloadSize-byte chunks and sends each as
// a 65-byte HID OUT report: [0x00][len][data...][padding].
// Thread-safe (protected by writeMu).
func (d *Device) writeRaw(raw []byte) error {
	report := make([]byte, hidReportSize+1) // hidapi: report_id(1) + data(64)
	report[0] = 0x00                        // report ID = 0 (no report IDs)

	d.writeMu.Lock()
	defer d.writeMu.Unlock()

	d.mu.Lock()
	port := d.port
	d.mu.Unlock()
	if port == nil {
		return ErrNotConnected
	}

	for len(raw) > 0 {
		n := len(raw)
		if n > hidPayloadSize {
			n = hidPayloadSize
		}
		report[1] = byte(n)
		copy(report[2:], raw[:n])
		// zero-pad
		for i := 2 + n; i <= hidReportSize; i++ {
			report[i] = 0
		}
		if _, err := port.Write(report); err != nil {
			return err
		}
		raw = raw[n:]
	}
	return nil
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

// drainPending closes all waiting Send() callers with ErrNotConnected.
func (d *Device) drainPending() {
	d.pendingMu.Lock()
	for seq, ch := range d.pending {
		close(ch)
		delete(d.pending, seq)
	}
	d.pendingMu.Unlock()
}

// shutdownRelay tears down any active relay connection.
func (d *Device) shutdownRelay() {
	d.relayMu.Lock()
	conn := d.relayConn
	d.relayConn = nil
	d.relayMu.Unlock()
	if conn != nil {
		conn.shutdown()
	}
}

// manage is the background goroutine that opens the HID device and keeps it
// alive.  On disconnect it drains pending requests, shuts down any relay
// connection, and retries after reconnectDelay.
func (d *Device) manage() {
	defer d.wg.Done()

	for {
		// Check for shutdown before attempting open.
		select {
		case <-d.ctx.Done():
			return
		default:
		}

		dev, err := hid.OpenFirst(d.vid, d.pid)
		if err != nil {
			slog.Debug("HID open failed, retrying",
				"vid", fmt.Sprintf("%04x", d.vid),
				"pid", fmt.Sprintf("%04x", d.pid),
				"err", err)
			select {
			case <-d.ctx.Done():
				return
			case <-time.After(reconnectDelay):
			}
			continue
		}

		d.mu.Lock()
		d.port = dev
		d.mu.Unlock()
		slog.Info("HID device connected",
			"vid", fmt.Sprintf("%04x", d.vid),
			"pid", fmt.Sprintf("%04x", d.pid))

		// Block until the read loop exits (disconnect or ctx cancel).
		d.runReadLoop(dev)

		// Clean up after disconnect.
		d.mu.Lock()
		d.port = nil
		d.mu.Unlock()
		dev.Close() //nolint:errcheck

		d.drainPending()
		d.shutdownRelay()

		select {
		case <-d.ctx.Done():
			return
		default:
			slog.Info("HID device disconnected, will reconnect",
				"vid", fmt.Sprintf("%04x", d.vid),
				"pid", fmt.Sprintf("%04x", d.pid))
		}
	}
}

// runReadLoop reads HID IN reports until an error or ctx cancellation.
// It does NOT close d.port — the caller (manage) owns that.
func (d *Device) runReadLoop(port *hid.Device) {
	parser := proto.NewParser(func(f proto.Frame) {
		if f.IsResp {
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
			d.dispatchPush(f)
		}
	})

	buf := make([]byte, hidReportSize)

	for {
		select {
		case <-d.ctx.Done():
			return
		default:
		}

		n, err := port.ReadWithTimeout(buf, readTimeout)
		if err != nil {
			if errors.Is(err, hid.ErrTimeout) || err.Error() == "Interrupted system call" {
				continue
			}
			select {
			case <-d.ctx.Done():
			default:
				slog.Warn("HID read error", "err", err)
			}
			return
		}
		if n < 1 {
			continue // timeout or empty report
		}

		// Report format: buf[0]=data_len, buf[1..data_len]=proto bytes
		dataLen := int(buf[0])
		if dataLen > hidPayloadSize || dataLen > n-1 {
			dataLen = n - 1
		}
		if dataLen > 0 {
			parser.Feed(buf[1 : 1+dataLen])
		}
	}
}
