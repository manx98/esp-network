// Package socks5 implements a SOCKS5 proxy server that tunnels TCP traffic
// through the ESP32's WiFi connection via USB serial.
package socks5

import (
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"net"

	"github.com/manx98/esp32-ctrl/device"
)

// reply codes (RFC 1928 §6)
const (
	repSuccess         byte = 0x00
	repGeneralFailure  byte = 0x01
	repConnRefused     byte = 0x05
	repCmdNotSupported byte = 0x07
	repAddrNotSupported byte = 0x08
)

// Server is a SOCKS5 proxy server.
type Server struct {
	dev *device.Device
}

// New creates a Server backed by dev.
func New(dev *device.Device) *Server {
	return &Server{dev: dev}
}

// ListenAndServe starts listening on addr and handles incoming SOCKS5 clients.
// It blocks until the listener is closed.
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
		go s.handleConn(conn)
	}
}

func (s *Server) handleConn(client net.Conn) {
	defer client.Close()

	// ── 1. Auth negotiation ───────────────────────────────────────────────────
	hdr := make([]byte, 2)
	if _, err := io.ReadFull(client, hdr); err != nil {
		return
	}
	if hdr[0] != 0x05 {
		return // not SOCKS5
	}
	nMethods := int(hdr[1])
	methods := make([]byte, nMethods)
	if _, err := io.ReadFull(client, methods); err != nil {
		return
	}
	// We accept no-authentication (method 0x00) only.
	hasNoAuth := false
	for _, m := range methods {
		if m == 0x00 {
			hasNoAuth = true
			break
		}
	}
	if !hasNoAuth {
		client.Write([]byte{0x05, 0xFF}) // no acceptable methods
		return
	}
	client.Write([]byte{0x05, 0x00}) // no auth selected

	// ── 2. Request ────────────────────────────────────────────────────────────
	req := make([]byte, 4)
	if _, err := io.ReadFull(client, req); err != nil {
		return
	}
	if req[0] != 0x05 {
		return
	}
	cmd := req[1]
	if cmd != 0x01 { // CONNECT only
		sendReply(client, repCmdNotSupported)
		return
	}
	atyp := req[3]

	var host string
	switch atyp {
	case 0x01: // IPv4
		addr := make([]byte, 4)
		if _, err := io.ReadFull(client, addr); err != nil {
			return
		}
		host = net.IP(addr).String()
	case 0x03: // domain name
		lenBuf := make([]byte, 1)
		if _, err := io.ReadFull(client, lenBuf); err != nil {
			return
		}
		domain := make([]byte, lenBuf[0])
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
		sendReply(client, repAddrNotSupported)
		return
	}

	portBuf := make([]byte, 2)
	if _, err := io.ReadFull(client, portBuf); err != nil {
		return
	}
	port := binary.BigEndian.Uint16(portBuf)

	slog.Debug("socks5 connect", "host", host, "port", port)

	// ── 3. Establish TCP via ESP32 ────────────────────────────────────────────
	tcpConn, err := s.dev.TCPConnect(host, port)
	if err != nil {
		slog.Warn("socks5 tcp connect failed", "host", host, "port", port, "err", err)
		sendReply(client, repConnRefused)
		return
	}
	// Success reply: BND.ADDR = 0.0.0.0, BND.PORT = 0
	sendReply(client, repSuccess)

	// ── 4. Bidirectional relay ────────────────────────────────────────────────
	done := make(chan struct{}, 2)

	// client → ESP32
	go func() {
		defer func() { done <- struct{}{} }()
		buf := make([]byte, 1020)
		for {
			n, err := client.Read(buf)
			if n > 0 {
				if werr := tcpConn.Write(buf[:n]); werr != nil {
					return
				}
			}
			if err != nil {
				return
			}
		}
	}()

	// ESP32 → client
	go func() {
		defer func() { done <- struct{}{} }()
		for {
			data, err := tcpConn.Read()
			if err != nil {
				return
			}
			if _, werr := client.Write(data); werr != nil {
				return
			}
		}
	}()

	<-done                    // one direction finished
	tcpConn.Close()           // unblocks the ESP32→client goroutine (recvCh closed)
	client.Close()            // unblocks the client→ESP32 goroutine (Read returns error)
	<-done                    // wait for the other direction to exit
	slog.Debug("socks5 relay done", "host", host, "port", port)
}

// sendReply sends a SOCKS5 reply with the given code and zero BND address.
func sendReply(w io.Writer, rep byte) {
	w.Write([]byte{0x05, rep, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})
}
