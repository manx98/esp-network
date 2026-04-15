// Package proxy implements a multiplexed client over the single ESP32 relay
// connection.  One TCPConn carries all virtual connections to the proxy server
// using the wire protocol defined in proxy/main.go:
//
//	[cmd:1][connID:4 LE][payloadLen:2 LE][payload...]
package proxy

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"sync"
	"time"

	"github.com/manx98/esp32-ctrl/device"
)

// Wire protocol commands (must match proxy/main.go).
const (
	cmdConnect byte = 0
	cmdData    byte = 1
	cmdClose   byte = 2
	cmdPing    byte = 3

	frameHdrLen       = 7 // [cmd:1][connID:4LE][len:2LE]
	heartbeatInterval = 10 * time.Second
	reconnectDelay    = 3 * time.Second
)

var (
	ErrNotConnected = errors.New("proxy relay not connected")
	ErrConnRejected = errors.New("connection rejected by proxy")
)

// ── ProxyConn ────────────────────────────────────────────────────────────────

// ProxyConn is one virtual connection multiplexed over the relay.
// Implements io.ReadWriteCloser.
type ProxyConn struct {
	id         uint32
	client     *Client
	recvCh     chan []byte
	last       []byte // unconsumed tail of last received chunk
	ctx        context.Context
	done       context.CancelFunc
	closeOnce  sync.Once
	connResult chan error // receives connect ack; buffered 1
}

// shutdown cancels the connection context and closes recvCh so Read returns EOF.
// Safe to call multiple times from multiple goroutines.
func (pc *ProxyConn) shutdown() {
	pc.closeOnce.Do(func() {
		pc.done()
		close(pc.recvCh)
	})
}

func (pc *ProxyConn) Read(p []byte) (int, error) {
	for len(pc.last) == 0 {
		select {
		case data, ok := <-pc.recvCh:
			if !ok {
				return 0, io.EOF
			}
			pc.last = data
		case <-pc.ctx.Done():
			return 0, io.EOF
		}
	}
	n := copy(p, pc.last)
	pc.last = pc.last[n:]
	return n, nil
}

func (pc *ProxyConn) Write(p []byte) (int, error) {
	if pc.ctx.Err() != nil {
		return 0, io.ErrClosedPipe
	}
	pc.client.writeFrame(cmdData, pc.id, p)
	return len(p), nil
}

func (pc *ProxyConn) Close() error {
	pc.client.removeConn(pc.id)
	pc.shutdown()
	pc.client.writeFrame(cmdClose, pc.id, nil)
	return nil
}

// deliver pushes incoming data into the recv channel.
// Uses recover to guard against the rare race where shutdown closes recvCh
// just as deliver is about to send.
func (pc *ProxyConn) deliver(data []byte) {
	defer func() { recover() }() //nolint:errcheck
	if pc.ctx.Err() != nil {
		return // already closed
	}
	cp := append([]byte(nil), data...)
	select {
	case pc.recvCh <- cp:
	default:
		slog.Warn("proxy: recv buffer full, dropping data", "connID", pc.id)
	}
}

// ── Client ───────────────────────────────────────────────────────────────────

// Client runs on top of a single relay TCPConn and multiplexes virtual
// connections using the proxy wire protocol.
type Client struct {
	conn   *device.TCPConn
	mu     sync.Mutex
	conns  map[uint32]*ProxyConn
	nextID uint32
	wLock  chan struct{} // serialises writes
	ctx    context.Context
	cancel context.CancelCauseFunc
}

func newClient(conn *device.TCPConn, ctx context.Context, cancel context.CancelCauseFunc) *Client {
	return &Client{
		conn:   conn,
		conns:  make(map[uint32]*ProxyConn),
		wLock:  make(chan struct{}, 1),
		ctx:    ctx,
		cancel: cancel,
	}
}

func (c *Client) writeFrame(cmd byte, connID uint32, data []byte) {
	select {
	case c.wLock <- struct{}{}:
		defer func() { <-c.wLock }()
	case <-c.ctx.Done():
		return
	}
	frame := make([]byte, frameHdrLen+len(data))
	frame[0] = cmd
	binary.LittleEndian.PutUint32(frame[1:], connID)
	binary.LittleEndian.PutUint16(frame[5:], uint16(len(data)))
	copy(frame[frameHdrLen:], data)
	if err := c.conn.Write(frame); err != nil {
		c.cancel(err)
	}
}

func (c *Client) removeConn(id uint32) {
	c.mu.Lock()
	delete(c.conns, id)
	c.mu.Unlock()
}

func (c *Client) getConn(id uint32) *ProxyConn {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.conns[id]
}

// ConnectRemote opens a virtual connection to host:port via the proxy server.
func (c *Client) ConnectRemote(host string, port uint16) (*ProxyConn, error) {
	hostB := []byte(host)
	payload := make([]byte, 1+len(hostB)+2)
	payload[0] = byte(len(hostB))
	copy(payload[1:], hostB)
	binary.LittleEndian.PutUint16(payload[1+len(hostB):], port)

	// Allocate connection slot.
	c.mu.Lock()
	id := c.nextID
	c.nextID++
	connCtx, connDone := context.WithCancel(c.ctx)
	pc := &ProxyConn{
		id:         id,
		client:     c,
		recvCh:     make(chan []byte, 64),
		ctx:        connCtx,
		done:       connDone,
		connResult: make(chan error, 1),
	}
	c.conns[id] = pc
	c.mu.Unlock()

	c.writeFrame(cmdConnect, id, payload)

	// Wait for connect ack from proxy server.
	select {
	case err := <-pc.connResult:
		if err != nil {
			c.removeConn(id)
			pc.shutdown()
			return nil, err
		}
		return pc, nil
	case <-c.ctx.Done():
		c.removeConn(id)
		pc.shutdown()
		return nil, context.Cause(c.ctx)
	}
}

// shutdownAll closes every active ProxyConn.  Called when the relay dies.
func (c *Client) shutdownAll() {
	c.mu.Lock()
	conns := make([]*ProxyConn, 0, len(c.conns))
	for _, pc := range c.conns {
		conns = append(conns, pc)
	}
	c.conns = make(map[uint32]*ProxyConn)
	c.mu.Unlock()
	for _, pc := range conns {
		pc.shutdown()
	}
}

// relayReader adapts device.TCPConn (chunk-based reads) to io.Reader so we
// can use io.ReadFull for clean frame reassembly.
type relayReader struct {
	conn *device.TCPConn
	buf  []byte
}

func (r *relayReader) Read(p []byte) (int, error) {
	for len(r.buf) == 0 {
		data, err := r.conn.Read()
		if err != nil {
			return 0, err
		}
		r.buf = data
	}
	n := copy(p, r.buf)
	r.buf = r.buf[n:]
	return n, nil
}

// readLoop reads proxy frames and dispatches them.  Blocks until the relay
// connection is lost, then calls shutdownAll.
func (c *Client) readLoop() {
	defer func() {
		c.cancel(ErrNotConnected)
		c.shutdownAll()
	}()

	r := &relayReader{conn: c.conn}
	hdr := make([]byte, frameHdrLen)

	for c.ctx.Err() == nil {
		if _, err := io.ReadFull(r, hdr); err != nil {
			if c.ctx.Err() == nil {
				slog.Warn("proxy: relay read error", "err", err)
				c.cancel(err)
			}
			return
		}

		cmd := hdr[0]
		connID := binary.LittleEndian.Uint32(hdr[1:5])
		payLen := int(binary.LittleEndian.Uint16(hdr[5:7]))

		var payload []byte
		if payLen > 0 {
			payload = make([]byte, payLen)
			if _, err := io.ReadFull(r, payload); err != nil {
				if c.ctx.Err() == nil {
					c.cancel(err)
				}
				return
			}
		}

		c.dispatch(cmd, connID, payload)
	}
}

func (c *Client) dispatch(cmd byte, connID uint32, payload []byte) {
	switch cmd {
	case cmdConnect:
		pc := c.getConn(connID)
		if pc == nil {
			return
		}
		var err error
		if len(payload) != 1 {
			err = errors.New("proxy: malformed connect ack")
		} else if payload[0] != 0 {
			err = fmt.Errorf("%w (status=%d)", ErrConnRejected, payload[0])
		}
		select {
		case pc.connResult <- err:
		default:
		}

	case cmdData:
		if pc := c.getConn(connID); pc != nil {
			pc.deliver(payload)
		}

	case cmdClose:
		if pc := c.getConn(connID); pc != nil {
			c.removeConn(connID)
			pc.shutdown()
		}
	case cmdPing:
	default:
		slog.Warn("proxy: unknown frame command", "cmd", cmd)
	}
}

// ── ProxyServer ──────────────────────────────────────────────────────────────

// ProxyServer manages the relay lifecycle and exposes Connect for callers.
// The relay to proxyHost:proxyPort is established eagerly and re-established
// automatically whenever it drops.
type ProxyServer struct {
	proxyHost string
	proxyPort uint16
	dev       *device.Device
	mu        sync.RWMutex
	client    *Client
	ctx       context.Context
	done      context.CancelFunc
}

// New creates and starts a ProxyServer.  Callers should use Connect to open
// virtual connections; Close shuts everything down.
func New(proxyHost string, proxyPort uint16, dev *device.Device) *ProxyServer {
	ps := &ProxyServer{
		proxyHost: proxyHost,
		proxyPort: proxyPort,
		dev:       dev,
	}
	ps.ctx, ps.done = context.WithCancel(context.Background())
	go ps.manage()
	return ps
}

// manage is the relay lifecycle goroutine.
func (ps *ProxyServer) manage() {
	for ps.ctx.Err() == nil {
		conn, err := ps.dev.TCPConnect(ps.proxyHost, ps.proxyPort)
		if err != nil {
			slog.Warn("proxy: relay connect failed", "host", ps.proxyHost,
				"port", ps.proxyPort, "err", err)
			select {
			case <-time.After(reconnectDelay):
			case <-ps.ctx.Done():
				return
			}
			continue
		}

		clientCtx, clientCancel := context.WithCancelCause(ps.ctx)
		client := newClient(conn, clientCtx, clientCancel)

		ps.mu.Lock()
		ps.client = client
		ps.mu.Unlock()

		slog.Info("proxy: relay up", "host", ps.proxyHost, "port", ps.proxyPort)

		// Heartbeat goroutine — keeps the relay alive.
		go func() {
			ticker := time.NewTicker(heartbeatInterval)
			defer ticker.Stop()
			for {
				select {
				case <-ticker.C:
					client.writeFrame(cmdPing, 0, nil)
				case <-clientCtx.Done():
					return
				}
			}
		}()

		// readLoop blocks until the relay dies.
		client.readLoop()

		ps.mu.Lock()
		if ps.client == client {
			ps.client = nil
		}
		ps.mu.Unlock()

		if ps.ctx.Err() != nil {
			return
		}
		slog.Info("proxy: relay lost, reconnecting", "after", reconnectDelay)
		select {
		case <-time.After(reconnectDelay):
		case <-ps.ctx.Done():
			return
		}
	}
}

// Connect opens a virtual connection to host:port via the proxy server.
// Returns an io.ReadWriteCloser ready for bidirectional data relay.
func (ps *ProxyServer) Connect(host string, port uint16) (io.ReadWriteCloser, error) {
	ps.mu.RLock()
	client := ps.client
	ps.mu.RUnlock()
	if client == nil || client.ctx.Err() != nil {
		return nil, ErrNotConnected
	}
	return client.ConnectRemote(host, port)
}

// Close shuts down the proxy server and its relay connection.
func (ps *ProxyServer) Close() {
	ps.done()
}
