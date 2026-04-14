// Package proxy implements a client that connects to an external proxy server
// and forwards SOCKS5 traffic through it.
package proxy

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"math"
	"sync"
	"time"

	"github.com/manx98/esp32-ctrl/device"
)

const (
	CmdProxyConnect byte = iota
	CmdProxyData
	CmdProxyClose
	CmdProxyPing

	heartbeatInterval = 10 * time.Second
)

type StatusCode byte

func (s StatusCode) Error() string {
	switch s {
	case StatusOK:
		return "OK"
	case StatusError:
		return "Error"
	case StatusRejected:
		return "Rejected"
	default:
		return fmt.Sprintf("Unknown(%0x)", byte(s))
	}
}

const (
	StatusOK StatusCode = iota
	StatusError
	StatusRejected
)

var (
	ErrNotConnected    = errors.New("proxy not connected")
	ErrConnectRejected = errors.New("connection rejected")
)

type ProxyServer struct {
	client    *Client
	proxyHost string
	proxyPort uint16
	dev       *device.Device
	mu        sync.Mutex
	reconnect chan struct{}
	ctx       context.Context
	done      context.CancelFunc
}

type Client struct {
	mu         sync.Mutex
	conn       *device.TCPConn
	ctx        context.Context
	cancel     context.CancelCauseFunc
	conns      map[uint32]*ProxyConn
	nextConnID uint32
	wCh        chan struct{}
}

type ProxyConn struct {
	id       uint32
	c        *Client
	recvCh   chan []byte
	ctx      context.Context
	done     context.CancelFunc
	last     []byte
	connCtx  context.Context
	connDone context.CancelCauseFunc
}

func (pc *ProxyConn) Read(p []byte) (int, error) {
	if len(pc.last) == 0 {
		select {
		case pc.last = <-pc.recvCh:
		case <-pc.ctx.Done():
			return 0, io.EOF
		}
	}
	var n int
	if len(pc.last) > len(p) {
		n = len(p)
	} else {
		n = len(pc.last)
	}
	copy(p, pc.last)
	pc.last = pc.last[n:]
	return n, nil
}

func (pc *ProxyConn) Write(p []byte) (n int, err error) {
	for len(p) > 0 {
		if pc.ctx.Err() != nil {
			return n, context.Cause(pc.ctx)
		}
		chunkSize := len(p)
		if chunkSize > math.MaxUint16 {
			chunkSize = math.MaxUint16
		}
		pc.c.writeCmd(CmdProxyData, pc.id, p[:chunkSize])
		n += chunkSize
		p = p[chunkSize:]
	}
	return
}

func (pc *ProxyConn) Close() error {
	pc.c.closeConn(pc.id)
	return nil
}

func (c *Client) ConnectRemote(host string, port uint16) (*ProxyConn, error) {
	hostBytes := []byte(host)
	if len(hostBytes) > 253 {
		return nil, errors.New("hostname too long")
	}

	connID := c.allocConnID()
	pc := &ProxyConn{
		id:     connID,
		c:      c,
		recvCh: make(chan []byte, 64),
	}
	pc.ctx, pc.done = context.WithCancel(c.ctx)
	pc.connCtx, pc.connDone = context.WithCancelCause(context.Background())
	c.mu.Lock()
	c.conns[connID] = pc
	c.mu.Unlock()

	payload := make([]byte, 1+len(hostBytes)+2)
	payload[0] = byte(len(hostBytes))
	copy(payload[1:], hostBytes)
	binary.LittleEndian.PutUint16(payload[1+len(hostBytes):], port)
	c.writeCmd(CmdProxyConnect, connID, payload)
	select {
	case <-pc.connCtx.Done():
		err := context.Cause(pc.connCtx)
		var code StatusCode
		if !errors.Is(code, StatusOK) {
			c.mu.Lock()
			delete(c.conns, connID)
			c.mu.Unlock()
			return nil, err
		}
	case <-c.ctx.Done():
		return nil, context.Cause(c.ctx)
	}
	slog.Debug("proxy: connected to remote", "connID", connID, "host", host, "port", port)
	return pc, nil
}

func (c *Client) allocConnID() uint32 {
	c.mu.Lock()
	id := c.nextConnID
	c.nextConnID++
	c.mu.Unlock()
	return id
}

func (c *Client) sendCommand(cmd byte, connID byte, payload []byte) ([]byte, error) {
	c.mu.Lock()
	conn := c.conn
	c.mu.Unlock()

	if conn == nil {
		return nil, ErrNotConnected
	}

	frame := make([]byte, 4+len(payload))
	frame[0] = cmd
	frame[1] = connID
	frame[2] = byte(len(payload) >> 8)
	frame[3] = byte(len(payload))
	copy(frame[4:], payload)

	// 发送命令
	err := conn.Write(frame)
	if err != nil {
		return nil, err
	}

	// 读取响应
	resp, err := conn.Read()
	if err != nil {
		return nil, err
	}

	return resp, nil
}

func (c *Client) writeCmd(cmd byte, connID uint32, data []byte) {
	select {
	case c.wCh <- struct{}{}:
		defer func() { <-c.wCh }()
	case <-c.ctx.Done():
		return
	}
	frame := make([]byte, 7+len(data))
	frame[0] = cmd
	binary.LittleEndian.PutUint32(frame[1:5], connID)
	binary.LittleEndian.PutUint16(frame[5:7], uint16(len(data)))
	copy(frame[7:], data)
	err := c.conn.Write(frame)
	if err != nil {
		slog.Warn("proxy send data failed", "connID", connID, "err", err)
		c.cancel(err)
	} else {
		slog.Debug("proxy sent data", "connID", connID, "len", len(data))
	}
}

func (c *Client) closeConn(connID uint32) {
	c.mu.Lock()
	pc := c.conns[connID]
	delete(c.conns, connID)
	c.mu.Unlock()
	if pc != nil {
		pc.done()
		c.writeCmd(CmdProxyClose, connID, nil)
	}
}

func (c *Client) heartbeat() {
	ticker := time.NewTicker(heartbeatInterval)
	defer ticker.Stop()
	for {
		select {
		case <-c.ctx.Done():
			return
		case <-ticker.C:
			c.writeCmd(CmdProxyPing, 0, nil)
		}
	}
}

func (c *Client) readLoop() {
	defer c.cancel(ErrNotConnected)
	header := make([]byte, 0, 7)
	var last []byte
	var payLoad []byte
	var err error
	for c.ctx.Err() == nil {
		if len(last) == 0 {
			last, err = c.conn.Read()
			if err != nil {
				slog.Warn("proxy read error", "err", err)
				c.cancel(err)
				return
			}
		}
		headerRemain := cap(header) - len(header)
		if headerRemain > 0 {
			if headerRemain >= len(last) {
				header = append(header, last...)
				last = nil
			} else {
				header = append(header, last[:headerRemain]...)
				last = last[headerRemain:]
				payLoad = make([]byte, 0, binary.LittleEndian.Uint16(header[5:]))
			}
			continue
		}
		payLoadRemain := cap(payLoad) - len(payLoad)
		if payLoadRemain >= 0 {
			if payLoadRemain >= len(last) {
				payLoad = append(payLoad, last...)
				last = nil
			} else {
				payLoad = append(payLoad, last[:payLoadRemain]...)
				c.handleData(header[0], binary.LittleEndian.Uint32(header[1:]), payLoad)
				header = header[:0]
				payLoad = nil
				last = last[payLoadRemain:]
			}
		}
	}
}

func (c *Client) handleData(cmd byte, connID uint32, data []byte) {
	c.mu.Lock()
	pc := c.conns[connID]
	c.mu.Unlock()
	switch cmd {
	case CmdProxyConnect:
		if pc != nil {
			if len(data) == 1 {
				pc.connDone(StatusCode(data[0]))
			} else {
				pc.connDone(errors.New("proxy connect: short response"))
			}
		}
	case CmdProxyData:
		if pc != nil {
			select {
			case pc.recvCh <- data:
			case <-pc.ctx.Done():
			default:
				slog.Warn("proxy recv buffer full", "connID", connID)
			}
		}
	case CmdProxyClose:
		if pc != nil {
			c.mu.Lock()
			delete(c.conns, connID)
			c.mu.Unlock()
			pc.done()
		}
	case CmdProxyPing:
		// heartbeat response, do nothing
	default:
		slog.Warn("unknown command", "cmd", cmd)
	}
}

func (c *ProxyServer) handleReconnect() {
	for {
		select {
		case <-c.reconnect:
		case <-c.ctx.Done():
			return
		}
		connect, err := c.dev.TCPConnect(c.proxyHost, c.proxyPort)
		if err != nil {
			slog.Warn("proxy reconnect failed", "err", err)
		} else {
			newClient := &Client{
				conn:  connect,
				conns: make(map[uint32]*ProxyConn),
				wCh:   make(chan struct{}, 1),
			}
			newClient.ctx, newClient.cancel = context.WithCancelCause(c.ctx)
			go newClient.readLoop()
			go newClient.heartbeat()
			c.mu.Lock()
			c.client = newClient
			c.mu.Unlock()
		}
	}
}

func (c *ProxyServer) getClient() (*Client, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.client == nil || c.client.ctx.Err() != nil {
		select {
		case c.reconnect <- struct{}{}:
		default:
		}
		return nil, ErrNotConnected
	}
	return c.client, nil
}

func (c *ProxyServer) Close() {
	c.done()
}

func (c *ProxyServer) Connect(host string, port uint16) (io.ReadWriteCloser, error) {
	client, err := c.getClient()
	if err != nil {
		return nil, err
	}
	return client.ConnectRemote(host, port)
}

func New(host string, prot uint16, dev *device.Device) *ProxyServer {
	server := &ProxyServer{
		proxyHost: host,
		dev:       dev,
		proxyPort: prot,
		reconnect: make(chan struct{}, 1),
	}
	server.ctx, server.done = context.WithCancel(context.Background())
	go server.handleReconnect()
	return server
}
