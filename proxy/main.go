package main

import (
	"bufio"
	"context"
	"encoding/base64"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"sync"
	"time"
)

const (
	CmdProxyConnect byte = iota
	CmdProxyData
	CmdProxyClose
	CmdProxyPing

	heartbeatInterval = 10 * time.Second

	// relayReadBuf: read buffer for target→relay path.  Large enough to fill
	// several TCP segments in one shot, reducing frame count.
	relayReadBuf = 32 * 1024

	// relayWriteQ: relay write queue depth.  Each entry is one serialised frame.
	// Acts as a pressure buffer so relayData goroutines never block on relay writes.
	relayWriteQ = 512

	// targetWriteQ: per-connection queue for ctrl→target writes.
	// Keeps the main read loop non-blocking even when a target is slow.
	targetWriteQ = 64
)

const (
	StatusOK       byte = 0
	StatusError    byte = 1
	StatusRejected byte = 2
)

// ── targetConn ───────────────────────────────────────────────────────────────

// targetConn wraps a TCP connection to a remote host.  It owns two goroutines:
//   - writeLoop: drains targetConn.writeQ → target TCP
//   - readLoop:  reads from target TCP   → relay write queue
//
// Both goroutines exit when ctx is cancelled.
type targetConn struct {
	id        uint32
	server    *ProxyServer
	conn      net.Conn
	writeQ    chan []byte
	ctx       context.Context
	done      context.CancelFunc
	closeOnce sync.Once
}

func newTargetConn(id uint32, srv *ProxyServer, conn net.Conn) *targetConn {
	ctx, done := context.WithCancel(srv.ctx)
	tc := &targetConn{
		id:     id,
		server: srv,
		conn:   conn,
		writeQ: make(chan []byte, targetWriteQ),
		ctx:    ctx,
		done:   done,
	}
	go tc.writeLoop()
	go tc.readLoop()
	return tc
}

// enqueue adds data to the target write queue (non-blocking).
// Drops data and logs a warning if the queue is full.
func (tc *targetConn) enqueue(data []byte) {
	cp := append([]byte(nil), data...)
	select {
	case tc.writeQ <- cp:
	case <-tc.ctx.Done():
	default:
		slog.Warn("target write queue full, dropping data", "connID", tc.id)
	}
}

// writeLoop drains writeQ into the target TCP connection.
func (tc *targetConn) writeLoop() {
	for {
		select {
		case data := <-tc.writeQ:
			if _, err := tc.conn.Write(data); err != nil {
				slog.Warn("target write error", "connID", tc.id, "err", err)
				tc.closeNotify()
				return
			}
		case <-tc.ctx.Done():
			return
		}
	}
}

// readLoop reads from the target and forwards data to the relay write queue.
func (tc *targetConn) readLoop() {
	defer tc.closeNotify()
	buf := make([]byte, relayReadBuf)
	for {
		n, err := tc.conn.Read(buf)
		if n > 0 {
			tc.server.writeFrame(CmdProxyData, tc.id, buf[:n])
		}
		if err != nil {
			break
		}
	}
}

// closeNotify is called when the connection closes from the target side.
// Sends CmdProxyClose to ctrl exactly once.
func (tc *targetConn) closeNotify() {
	tc.closeOnce.Do(func() {
		tc.done()
		_ = tc.conn.Close()
		tc.server.removeConn(tc.id)
		tc.server.writeFrame(CmdProxyClose, tc.id, nil)
	})
}

// closeByRequest is called when ctrl explicitly sends CmdProxyClose.
// Does NOT send CmdProxyClose back (ctrl already knows).
func (tc *targetConn) closeByRequest() {
	tc.closeOnce.Do(func() {
		tc.done()
		_ = tc.conn.Close()
	})
}

// ── ProxyServer ───────────────────────────────────────────────────────────────

// ProxyServer handles one relay connection from the ctrl side.
//
// Write path (target → ctrl):
//   readLoop goroutines → writeFrame() enqueues → writeLoop() batches & flushes
//
// Read path (ctrl → target):
//   Run() reads frames → dispatch() → tc.enqueue() (non-blocking)
type ProxyServer struct {
	conn   net.Conn
	mu     sync.Mutex
	conns  map[uint32]*targetConn
	writeQ chan []byte // serialised relay-write queue
	ctx    context.Context
	cause  context.CancelCauseFunc
}

func New(conn net.Conn) *ProxyServer {
	ctx, cause := context.WithCancelCause(context.Background())
	s := &ProxyServer{
		conn:   conn,
		conns:  make(map[uint32]*targetConn),
		writeQ: make(chan []byte, relayWriteQ),
		ctx:    ctx,
		cause:  cause,
	}
	go s.writeLoop()
	return s
}

// writeLoop is the single goroutine that writes to the relay TCP connection.
// It batches multiple queued frames into one bufio flush to reduce syscalls.
func (s *ProxyServer) writeLoop() {
	w := bufio.NewWriterSize(s.conn, 64*1024)
	for {
		// Block until at least one frame is ready.
		select {
		case frame, ok := <-s.writeQ:
			if !ok {
				return
			}
			w.Write(frame) //nolint:errcheck
		case <-s.ctx.Done():
			return
		}
		// Drain any additional frames that arrived while we were writing.
		for drained := false; !drained; {
			select {
			case frame := <-s.writeQ:
				w.Write(frame) //nolint:errcheck
			default:
				drained = true
			}
		}
		if err := w.Flush(); err != nil {
			s.cause(err)
			return
		}
	}
}

// writeFrame serialises a proxy frame and enqueues it for relay transmission.
// Never blocks on TCP; back-pressure is absorbed by writeQ depth.
func (s *ProxyServer) writeFrame(cmd byte, connID uint32, data []byte) {
	frame := make([]byte, 7+len(data))
	frame[0] = cmd
	binary.LittleEndian.PutUint32(frame[1:], connID)
	binary.LittleEndian.PutUint16(frame[5:], uint16(len(data)))
	copy(frame[7:], data)
	select {
	case s.writeQ <- frame:
	case <-s.ctx.Done():
	}
}

func (s *ProxyServer) addConn(tc *targetConn) {
	s.mu.Lock()
	s.conns[tc.id] = tc
	s.mu.Unlock()
}

func (s *ProxyServer) removeConn(id uint32) {
	s.mu.Lock()
	delete(s.conns, id)
	s.mu.Unlock()
}

func (s *ProxyServer) getConn(id uint32) *targetConn {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.conns[id]
}

func (s *ProxyServer) cleanup() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, tc := range s.conns {
		tc.done()
		_ = tc.conn.Close()
	}
	s.conns = make(map[uint32]*targetConn)
}

// Run reads frames from the relay and dispatches them.
// This is the only goroutine reading from s.conn; it never blocks on writes.
func (s *ProxyServer) Run() {
	slog.Info("relay connected", "addr", s.conn.RemoteAddr())
	defer s.cleanup()

	// Heartbeat: keep the relay TCP connection alive.
	go func() {
		ticker := time.NewTicker(heartbeatInterval)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				s.writeFrame(CmdProxyPing, 0, nil)
			case <-s.ctx.Done():
				return
			}
		}
	}()

	hdr := make([]byte, 7)
	for s.ctx.Err() == nil {
		if _, err := io.ReadFull(s.conn, hdr); err != nil {
			slog.Info("relay read error", "err", err)
			s.cause(err)
			return
		}
		cmd := hdr[0]
		connID := binary.LittleEndian.Uint32(hdr[1:5])
		payLen := int(binary.LittleEndian.Uint16(hdr[5:7]))

		var payload []byte
		if payLen > 0 {
			payload = make([]byte, payLen)
			if _, err := io.ReadFull(s.conn, payload); err != nil {
				slog.Info("relay payload read error", "err", err)
				s.cause(err)
				return
			}
		}

		s.dispatch(cmd, connID, payload)
	}
}

func (s *ProxyServer) dispatch(cmd byte, connID uint32, payload []byte) {
	switch cmd {
	case CmdProxyConnect:
		// Dial is blocking (DNS + TCP handshake) — run in goroutine so
		// the main read loop is never stalled by slow connects.
		go s.handleConnect(connID, payload)

	case CmdProxyData:
		if tc := s.getConn(connID); tc != nil {
			tc.enqueue(payload) // non-blocking
		}

	case CmdProxyClose:
		if tc := s.getConn(connID); tc != nil {
			s.removeConn(connID)
			tc.closeByRequest()
		}

	case CmdProxyPing:
		s.writeFrame(CmdProxyPing, 0, nil)

	default:
		slog.Warn("unknown command", "cmd", cmd)
	}
}

func (s *ProxyServer) handleConnect(connID uint32, payload []byte) {
	if len(payload) < 3 {
		slog.Warn("connect: payload too short", "connID", connID)
		s.writeFrame(CmdProxyConnect, connID, []byte{StatusError})
		return
	}
	hostLen := int(payload[0])
	if len(payload) != 1+hostLen+2 {
		slog.Warn("connect: malformed payload", "connID", connID)
		s.writeFrame(CmdProxyConnect, connID, []byte{StatusError})
		return
	}
	host := string(payload[1 : 1+hostLen])
	port := binary.LittleEndian.Uint16(payload[1+hostLen:])
	addr := net.JoinHostPort(host, fmt.Sprintf("%d", port))

	slog.Debug("connect", "connID", connID, "addr", addr)
	target, err := proxyDial(addr, 10*time.Second)
	if err != nil {
		slog.Warn("dial failed", "addr", addr, "err", err)
		s.writeFrame(CmdProxyConnect, connID, []byte{StatusRejected})
		return
	}

	// Register BEFORE sending OK so ctrl can't send data before we're ready.
	tc := newTargetConn(connID, s, target)
	s.addConn(tc)
	s.writeFrame(CmdProxyConnect, connID, []byte{StatusOK})
	slog.Info("connected", "connID", connID, "addr", addr)
}

// ── System-proxy-aware dialer ─────────────────────────────────────────────────

// proxyDial connects to target ("host:port") respecting the system proxy
// settings read from the standard environment variables:
//
//	HTTP_PROXY / http_proxy
//	HTTPS_PROXY / https_proxy
//	NO_PROXY / no_proxy
//
// Supported proxy schemes: http (CONNECT tunnel), https (CONNECT tunnel),
// socks5, socks5h.  Proxy authentication (Basic for HTTP, user:pass for
// SOCKS5) is taken from the proxy URL's userinfo.
func proxyDial(target string, timeout time.Duration) (net.Conn, error) {
	// http.ProxyFromEnvironment parses HTTP_PROXY/HTTPS_PROXY/NO_PROXY
	// correctly (including bypass lists) for any URL.  We pass a synthetic
	// HTTPS request so HTTPS_PROXY is checked preferentially.
	req, _ := http.NewRequest("CONNECT", "https://"+target, nil)
	proxyURL, err := http.ProxyFromEnvironment(req)
	if err != nil {
		return nil, fmt.Errorf("proxy env: %w", err)
	}
	if proxyURL == nil {
		// Direct connection — no proxy configured or target is in NO_PROXY.
		return net.DialTimeout("tcp", target, timeout)
	}

	slog.Debug("using system proxy", "proxy", proxyURL.Redacted(), "target", target)

	switch proxyURL.Scheme {
	case "http", "https":
		return dialHTTPConnect(proxyURL, target, timeout)
	case "socks5", "socks5h":
		return dialSOCKS5(proxyURL, target, timeout)
	default:
		slog.Warn("unknown proxy scheme, falling back to direct",
			"scheme", proxyURL.Scheme)
		return net.DialTimeout("tcp", target, timeout)
	}
}

// dialHTTPConnect tunnels a TCP connection through an HTTP(S) CONNECT proxy.
func dialHTTPConnect(proxyURL *url.URL, target string, timeout time.Duration) (net.Conn, error) {
	conn, err := net.DialTimeout("tcp", proxyURL.Host, timeout)
	if err != nil {
		return nil, fmt.Errorf("http proxy dial: %w", err)
	}

	// Build CONNECT request.
	req, _ := http.NewRequest("CONNECT", "//"+target, nil)
	req.Host = target
	if u := proxyURL.User; u != nil {
		pass, _ := u.Password()
		creds := base64.StdEncoding.EncodeToString([]byte(u.Username() + ":" + pass))
		req.Header.Set("Proxy-Authorization", "Basic "+creds)
	}
	req.Header.Set("User-Agent", "esp32-proxy/1.0")
	if err := req.Write(conn); err != nil {
		conn.Close()
		return nil, fmt.Errorf("http proxy write: %w", err)
	}

	// Read the CONNECT response.  Use a temporary bufio.Reader; wrap the
	// connection so any bytes already consumed by the reader are replayed.
	br := bufio.NewReader(conn)
	resp, err := http.ReadResponse(br, req)
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("http proxy response: %w", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		conn.Close()
		return nil, fmt.Errorf("http proxy CONNECT: %s", resp.Status)
	}

	// If bufio.Reader buffered any bytes past the response headers, wrap the
	// connection so those bytes are returned by future Read calls.
	if br.Buffered() > 0 {
		peeked, _ := br.Peek(br.Buffered())
		return &prefixConn{Conn: conn, r: io.MultiReader(
			newByteReader(peeked), conn,
		)}, nil
	}
	return conn, nil
}

// dialSOCKS5 connects via a SOCKS5 proxy (RFC 1928 + RFC 1929 user/pass auth).
func dialSOCKS5(proxyURL *url.URL, target string, timeout time.Duration) (net.Conn, error) {
	conn, err := net.DialTimeout("tcp", proxyURL.Host, timeout)
	if err != nil {
		return nil, fmt.Errorf("socks5 dial: %w", err)
	}
	conn.SetDeadline(time.Now().Add(timeout)) //nolint:errcheck
	defer conn.SetDeadline(time.Time{})       //nolint:errcheck

	host, portStr, err := net.SplitHostPort(target)
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("socks5: parse target: %w", err)
	}
	port, _ := strconv.Atoi(portStr)

	// ── Auth method negotiation ──
	var authMethod byte = 0x00 // no auth
	if proxyURL.User != nil {
		authMethod = 0x02 // username/password
	}
	if _, err := conn.Write([]byte{0x05, 0x01, authMethod}); err != nil {
		conn.Close()
		return nil, fmt.Errorf("socks5: greeting: %w", err)
	}
	resp := make([]byte, 2)
	if _, err := io.ReadFull(conn, resp); err != nil || resp[0] != 0x05 {
		conn.Close()
		return nil, fmt.Errorf("socks5: bad greeting response")
	}
	if resp[1] == 0xFF {
		conn.Close()
		return nil, fmt.Errorf("socks5: no acceptable auth method")
	}

	// ── Username/password auth (RFC 1929) ──
	if resp[1] == 0x02 {
		u := ""
		p := ""
		if proxyURL.User != nil {
			u = proxyURL.User.Username()
			p, _ = proxyURL.User.Password()
		}
		auth := append([]byte{0x01, byte(len(u))}, []byte(u)...)
		auth = append(auth, byte(len(p)))
		auth = append(auth, []byte(p)...)
		if _, err := conn.Write(auth); err != nil {
			conn.Close()
			return nil, fmt.Errorf("socks5: auth write: %w", err)
		}
		authResp := make([]byte, 2)
		if _, err := io.ReadFull(conn, authResp); err != nil || authResp[1] != 0x00 {
			conn.Close()
			return nil, fmt.Errorf("socks5: authentication failed")
		}
	}

	// ── CONNECT request ──
	hostB := []byte(host)
	req := append([]byte{0x05, 0x01, 0x00, 0x03, byte(len(hostB))}, hostB...)
	req = append(req, byte(port>>8), byte(port&0xFF))
	if _, err := conn.Write(req); err != nil {
		conn.Close()
		return nil, fmt.Errorf("socks5: connect write: %w", err)
	}

	// ── CONNECT response ──
	connResp := make([]byte, 4)
	if _, err := io.ReadFull(conn, connResp); err != nil {
		conn.Close()
		return nil, fmt.Errorf("socks5: connect response: %w", err)
	}
	if connResp[1] != 0x00 {
		conn.Close()
		return nil, fmt.Errorf("socks5: connect failed (rep=%d)", connResp[1])
	}
	// Discard the bound address returned by the server.
	var skipLen int
	switch connResp[3] {
	case 0x01:
		skipLen = 4 + 2 // IPv4 + port
	case 0x03:
		lb := make([]byte, 1)
		if _, err := io.ReadFull(conn, lb); err != nil {
			conn.Close()
			return nil, fmt.Errorf("socks5: bound addr len: %w", err)
		}
		skipLen = int(lb[0]) + 2 // domain + port
	case 0x04:
		skipLen = 16 + 2 // IPv6 + port
	}
	if _, err := io.ReadFull(conn, make([]byte, skipLen)); err != nil {
		conn.Close()
		return nil, fmt.Errorf("socks5: bound addr: %w", err)
	}

	return conn, nil
}

// prefixConn replays already-buffered bytes before delegating to the
// underlying connection.  Used after HTTP CONNECT response parsing.
type prefixConn struct {
	net.Conn
	r io.Reader
}

func (c *prefixConn) Read(p []byte) (int, error) { return c.r.Read(p) }

// newByteReader returns an io.Reader over a copy of b.
func newByteReader(b []byte) io.Reader {
	cp := make([]byte, len(b))
	copy(cp, b)
	return &byteReader{b: cp}
}

type byteReader struct{ b []byte }

func (r *byteReader) Read(p []byte) (int, error) {
	if len(r.b) == 0 {
		return 0, io.EOF
	}
	n := copy(p, r.b)
	r.b = r.b[n:]
	return n, nil
}

func main() {
	addr := flag.String("addr", ":11080", "Proxy server listen address")
	flag.Parse()

	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	})))

	ln, err := net.Listen("tcp", *addr)
	if err != nil {
		slog.Error("listen failed", "err", err)
		os.Exit(1)
	}
	slog.Info("proxy server listening", "addr", *addr)
	for {
		conn, err := ln.Accept()
		if err != nil {
			slog.Error("accept failed", "err", err)
			os.Exit(1)
		}
		go New(conn).Run()
	}
}
