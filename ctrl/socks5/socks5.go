// Package socks5 implements a SOCKS5 proxy server that forwards traffic
// through the external proxy server via the ESP32 relay connection.
package socks5

import (
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"net"

	"github.com/manx98/esp32-ctrl/proxy"
)

// connector is satisfied by proxy.ProxyServer.
type connector interface {
	Connect(host string, port uint16) (io.ReadWriteCloser, error)
}

// Server is a SOCKS5 proxy that tunnels connections through the proxy server.
type Server struct {
	proxy connector
}

// New creates a SOCKS5 server backed by the given proxy server.
func New(p *proxy.ProxyServer) *Server {
	return &Server{proxy: p}
}

// ListenAndServe starts listening on addr and handles connections.
func (s *Server) ListenAndServe(addr string) error {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("socks5 listen %s: %w", addr, err)
	}
	defer ln.Close()
	slog.Info("socks5 proxy listening", "addr", addr)

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

	// ── Handshake: auth method negotiation ──
	hdr := make([]byte, 2)
	if _, err := io.ReadFull(client, hdr); err != nil || hdr[0] != 0x05 {
		return
	}
	methods := make([]byte, hdr[1])
	if _, err := io.ReadFull(client, methods); err != nil {
		return
	}
	// Accept no-auth only.
	for _, m := range methods {
		if m == 0x00 {
			client.Write([]byte{0x05, 0x00})
			goto request
		}
	}
	client.Write([]byte{0x05, 0xFF})
	return

request:
	// ── Request ──
	req := make([]byte, 4)
	if _, err := io.ReadFull(client, req); err != nil || req[0] != 0x05 {
		return
	}
	if req[1] != 0x01 { // CONNECT only
		sendReply(client, 0x07) // command not supported
		return
	}

	var host string
	switch req[3] {
	case 0x01: // IPv4
		addr := make([]byte, 4)
		if _, err := io.ReadFull(client, addr); err != nil {
			return
		}
		host = net.IP(addr).String()
	case 0x03: // domain
		lbuf := make([]byte, 1)
		if _, err := io.ReadFull(client, lbuf); err != nil {
			return
		}
		domain := make([]byte, lbuf[0])
		if _, err := io.ReadFull(client, domain); err != nil {
			return
		}
		host = string(domain)
	case 0x04: // IPv6
		addr := make([]byte, 16)
		if _, err := io.ReadFull(client, addr); err != nil {
			return
		}
		host = net.IP(addr).String()
	default:
		sendReply(client, 0x08) // address type not supported
		return
	}

	portBuf := make([]byte, 2)
	if _, err := io.ReadFull(client, portBuf); err != nil {
		return
	}
	port := binary.BigEndian.Uint16(portBuf)

	slog.Debug("socks5 connect", "host", host, "port", port)

	target, err := s.proxy.Connect(host, port)
	if err != nil {
		slog.Warn("socks5: proxy connect failed", "host", host, "port", port, "err", err)
		sendReply(client, 0x05) // connection refused
		return
	}
	defer target.Close()

	sendReply(client, 0x00) // success

	// ── Relay ──
	done := make(chan struct{}, 2)
	go func() {
		defer func() { done <- struct{}{} }()
		io.Copy(target, client) //nolint:errcheck
	}()
	go func() {
		defer func() { done <- struct{}{} }()
		io.Copy(client, target) //nolint:errcheck
	}()
	<-done
}

func sendReply(w io.Writer, rep byte) {
	// VER=5, REP, RSV=0, ATYP=1 (IPv4), BND.ADDR=0.0.0.0, BND.PORT=0
	w.Write([]byte{0x05, rep, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) //nolint:errcheck
}
