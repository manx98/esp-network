// Package device manages the serial connection to the ESP32 and provides
// a request/response API matched by sequence number.
package device

import (
	"context"
	"errors"
	"fmt"
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
)

var (
	ErrNotConnected = errors.New("device not connected")
	ErrTimeout      = errors.New("response timeout")
)

// Device represents a connected ESP32 over USB-CDC serial.
type Device struct {
	portName string
	baudRate int

	mu   sync.Mutex
	port serial.Port

	seqCounter atomic.Uint32

	pendingMu sync.Mutex
	pending   map[byte]chan proto.Frame

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

	// Drain all pending waiters.
	d.pendingMu.Lock()
	for seq, ch := range d.pending {
		close(ch)
		delete(d.pending, seq)
	}
	d.pendingMu.Unlock()
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
	if _, err := port.Write(raw); err != nil {
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

// readLoop runs in its own goroutine, feeding serial bytes into the parser
// and dispatching completed response frames to the matching pending channel.
func (d *Device) readLoop() {
	defer d.wg.Done()

	parser := proto.NewParser(func(f proto.Frame) {
		if !f.IsResp {
			return
		}
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
				// Mark port as closed and exit; caller can reopen.
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
