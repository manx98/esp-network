// Package socks5 implements a combined SOCKS5 + HTTP/HTTPS proxy server that
// forwards traffic through the external proxy server via the ESP32 relay.
// Both protocols are served on the same port; the first byte determines which:
//   - 0x05 → SOCKS5
//   - anything else → HTTP (CONNECT for HTTPS, plain method for HTTP)
package socks5

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"strconv"

	"github.com/manx98/esp32-ctrl/proxy"
)

// connector is satisfied by proxy.ProxyServer.
type connector interface {
	Connect(host string, port uint16) (io.ReadWriteCloser, error)
}

// Server handles SOCKS5, HTTP CONNECT, and plain HTTP proxy on the same port.
type Server struct {
	proxy connector
}

// New creates a Server backed by the given proxy server.
func New(p *proxy.ProxyServer) *Server {
	return &Server{proxy: p}
}

// ListenAndServe starts listening on addr and handles connections.
func (s *Server) ListenAndServe(addr string) error {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("proxy listen %s: %w", addr, err)
	}
	defer ln.Close()
	slog.Info("proxy listening (SOCKS5+HTTP)", "addr", addr)

	for {
		conn, err := ln.Accept()
		if err != nil {
			return err
		}
		go s.handle(conn)
	}
}

func (s *Server) handle(client net.Conn) {
	defer client.Close()

	// Sniff the first byte to detect protocol.
	first := make([]byte, 1)
	if _, err := io.ReadFull(client, first); err != nil {
		return
	}

	// Replay the consumed byte in front of the connection.
	r := io.MultiReader(bytes.NewReader(first), client)

	if first[0] == 0x05 {
		s.handleSOCKS5(r, client)
	} else {
		s.handleHTTP(r, client)
	}
}

// ── SOCKS5 ───────────────────────────────────────────────────────────────────

func (s *Server) handleSOCKS5(r io.Reader, w io.Writer) {
	// Auth negotiation: [VER:1][NMETHODS:1][METHODS:N]
	hdr := make([]byte, 2)
	if _, err := io.ReadFull(r, hdr); err != nil || hdr[0] != 0x05 {
		return
	}
	methods := make([]byte, hdr[1])
	if _, err := io.ReadFull(r, methods); err != nil {
		return
	}
	for _, m := range methods {
		if m == 0x00 {
			w.Write([]byte{0x05, 0x00}) //nolint:errcheck
			goto request
		}
	}
	w.Write([]byte{0x05, 0xFF}) //nolint:errcheck
	return

request:
	req := make([]byte, 4)
	if _, err := io.ReadFull(r, req); err != nil || req[0] != 0x05 {
		return
	}
	if req[1] != 0x01 { // CONNECT only
		w.Write(socks5Reply(0x07)) //nolint:errcheck
		return
	}

	var host string
	switch req[3] {
	case 0x01: // IPv4
		addr := make([]byte, 4)
		if _, err := io.ReadFull(r, addr); err != nil {
			return
		}
		host = net.IP(addr).String()
	case 0x03: // domain
		lbuf := make([]byte, 1)
		if _, err := io.ReadFull(r, lbuf); err != nil {
			return
		}
		domain := make([]byte, lbuf[0])
		if _, err := io.ReadFull(r, domain); err != nil {
			return
		}
		host = string(domain)
	case 0x04: // IPv6
		addr := make([]byte, 16)
		if _, err := io.ReadFull(r, addr); err != nil {
			return
		}
		host = net.IP(addr).String()
	default:
		w.Write(socks5Reply(0x08)) //nolint:errcheck
		return
	}

	portBuf := make([]byte, 2)
	if _, err := io.ReadFull(r, portBuf); err != nil {
		return
	}
	port := binary.BigEndian.Uint16(portBuf)

	slog.Debug("socks5 connect", "host", host, "port", port)

	target, err := s.proxy.Connect(host, port)
	if err != nil {
		slog.Warn("socks5: proxy connect failed", "host", host, "port", port, "err", err)
		w.Write(socks5Reply(0x05)) //nolint:errcheck
		return
	}
	defer target.Close()

	w.Write(socks5Reply(0x00)) //nolint:errcheck
	relayBidi(target, r, w)
}

func socks5Reply(rep byte) []byte {
	// VER=5, REP, RSV=0, ATYP=1 (IPv4), BND.ADDR=0.0.0.0, BND.PORT=0
	return []byte{0x05, rep, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
}

// ── HTTP / HTTPS ─────────────────────────────────────────────────────────────

func (s *Server) handleHTTP(r io.Reader, client net.Conn) {
	br := bufio.NewReader(r)
	req, err := http.ReadRequest(br)
	if err != nil {
		return
	}
	if req.Method == "CONNECT" {
		s.handleHTTPConnect(req, br, client)
	} else {
		s.handleHTTPForward(req, br, client)
	}
}

// handleHTTPConnect handles HTTPS tunnelling via HTTP CONNECT.
func (s *Server) handleHTTPConnect(req *http.Request, br *bufio.Reader, client net.Conn) {
	host, portStr, err := net.SplitHostPort(req.Host)
	if err != nil {
		// No port in Host (unusual but handle it).
		host = req.Host
		portStr = "443"
	}
	port, err := strconv.Atoi(portStr)
	if err != nil || port <= 0 || port > 65535 {
		client.Write([]byte("HTTP/1.1 400 Bad Request\r\n\r\n")) //nolint:errcheck
		return
	}

	slog.Debug("http connect", "host", host, "port", port)

	target, err := s.proxy.Connect(host, uint16(port))
	if err != nil {
		slog.Warn("http: proxy connect failed", "host", host, "port", port, "err", err)
		client.Write([]byte("HTTP/1.1 502 Bad Gateway\r\n\r\n")) //nolint:errcheck
		return
	}
	defer target.Close()

	client.Write([]byte("HTTP/1.1 200 Connection Established\r\n\r\n")) //nolint:errcheck
	// br may have buffered bytes beyond the CONNECT request headers; drain them.
	relayBidi(target, br, client)
}

// handleHTTPForward handles plain HTTP proxy requests (GET/POST/etc).
// Opens a new upstream connection per request; loops for keep-alive clients.
func (s *Server) handleHTTPForward(req *http.Request, br *bufio.Reader, client net.Conn) {
	for req != nil {
		if req.URL == nil || req.URL.Host == "" {
			client.Write([]byte("HTTP/1.1 400 Bad Request\r\n\r\n")) //nolint:errcheck
			return
		}

		host := req.URL.Hostname()
		portStr := req.URL.Port()
		if portStr == "" {
			portStr = "80"
		}
		port, err := strconv.Atoi(portStr)
		if err != nil || port <= 0 || port > 65535 {
			client.Write([]byte("HTTP/1.1 400 Bad Request\r\n\r\n")) //nolint:errcheck
			return
		}

		slog.Debug("http forward", "method", req.Method, "host", host, "port", port)

		target, err := s.proxy.Connect(host, uint16(port))
		if err != nil {
			slog.Warn("http: proxy connect failed", "host", host, "port", port, "err", err)
			client.Write([]byte("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")) //nolint:errcheck
			return
		}

		// Rewrite for direct forwarding: use path-only URI, strip proxy headers,
		// force Connection: close so we can cleanly read one response.
		req.RequestURI = req.URL.RequestURI()
		req.Header.Del("Proxy-Connection")
		req.Header.Del("Proxy-Authorization")
		req.Header.Set("Connection", "close")
		req.Close = true

		if err := req.Write(target); err != nil {
			target.Close()
			return
		}

		resp, err := http.ReadResponse(bufio.NewReader(target), req)
		target.Close()
		if err != nil {
			return
		}
		resp.Close = true
		resp.Write(client) //nolint:errcheck
		resp.Body.Close()

		// Attempt to read the next pipelined request from the client.
		req, err = http.ReadRequest(br)
		if err != nil {
			return // EOF or client closed — normal exit
		}
	}
}

// ── helpers ──────────────────────────────────────────────────────────────────

// relayBidi copies data in both directions until either side closes.
func relayBidi(target io.ReadWriteCloser, cr io.Reader, cw io.Writer) {
	done := make(chan struct{}, 2)
	go func() {
		defer func() { done <- struct{}{} }()
		io.Copy(target, cr) //nolint:errcheck
	}()
	go func() {
		defer func() { done <- struct{}{} }()
		io.Copy(cw, target) //nolint:errcheck
	}()
	<-done
}
