// Package socks5 implements a SOCKS5 proxy server that tunnels TCP traffic
// either through the ESP32's WiFi or through an external proxy server.
package socks5

import (
	"encoding/binary"
	"fmt"
	"io"
	"log/slog"
	"net"

	"github.com/manx98/esp32-ctrl/device"
	"github.com/manx98/esp32-ctrl/proxy"
)

const (
	repSuccess          byte = 0x00
	repGeneralFailure   byte = 0x01
	repConnRefused      byte = 0x05
	repCmdNotSupported  byte = 0x07
	repAddrNotSupported byte = 0x08
)

type tcpConn interface {
	Read(p []byte) (int, error)
	Write(p []byte) (int, error)
	Close() error
}

type deviceTCPConn struct {
	conn *device.TCPConn
}

func (d *deviceTCPConn) Read(p []byte) (int, error) {
	data, err := d.conn.Read()
	if err != nil {
		return 0, err
	}
	n := copy(p, data)
	return n, nil
}

func (d *deviceTCPConn) Write(p []byte) (int, error) {
	err := d.conn.Write(p)
	if err != nil {
		return 0, err
	}
	return len(p), nil
}

func (d *deviceTCPConn) Close() error {
	return d.conn.Close()
}

type Server struct {
	dev    *device.Device
	proxyC *proxy.ProxyServer
}

func New(dev *device.Device) *Server {
	return &Server{dev: dev}
}

func NewWithProxy(dev *device.Device, proxyHost string, proxyPort uint16) *Server {
	p := proxy.New(proxyHost, proxyPort, dev)
	return &Server{dev: dev, proxyC: p}
}

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

	hdr := make([]byte, 2)
	if _, err := io.ReadFull(client, hdr); err != nil {
		return
	}
	if hdr[0] != 0x05 {
		return
	}
	nMethods := int(hdr[1])
	methods := make([]byte, nMethods)
	if _, err := io.ReadFull(client, methods); err != nil {
		return
	}
	hasNoAuth := false
	for _, m := range methods {
		if m == 0x00 {
			hasNoAuth = true
			break
		}
	}
	if !hasNoAuth {
		client.Write([]byte{0x05, 0xFF})
		return
	}
	client.Write([]byte{0x05, 0x00})

	req := make([]byte, 4)
	if _, err := io.ReadFull(client, req); err != nil {
		return
	}
	if req[0] != 0x05 {
		return
	}
	cmd := req[1]
	if cmd != 0x01 {
		sendReply(client, repCmdNotSupported)
		return
	}
	atyp := req[3]

	var host string
	switch atyp {
	case 0x01:
		addr := make([]byte, 4)
		if _, err := io.ReadFull(client, addr); err != nil {
			return
		}
		host = net.IP(addr).String()
	case 0x03:
		lenBuf := make([]byte, 1)
		if _, err := io.ReadFull(client, lenBuf); err != nil {
			return
		}
		domain := make([]byte, lenBuf[0])
		if _, err := io.ReadFull(client, domain); err != nil {
			return
		}
		host = string(domain)
	case 0x04:
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

	var target io.ReadWriteCloser
	var err error

	if s.proxyC != nil {
		target, err = s.proxyC.Connect(host, port)
	} else {
		var dc *device.TCPConn
		dc, err = s.dev.TCPConnect(host, port)
		if err == nil {
			target = &deviceTCPConn{conn: dc}
		}
	}

	if err != nil {
		slog.Warn("socks5 tcp connect failed", "host", host, "port", port, "err", err)
		sendReply(client, repConnRefused)
		return
	}
	defer target.Close()

	sendReply(client, repSuccess)

	done := make(chan struct{}, 2)

	go func() {
		defer func() { done <- struct{}{} }()
		_, _ = io.CopyBuffer(target, client, make([]byte, 1020))
	}()

	go func() {
		defer func() { done <- struct{}{} }()
		_, _ = io.CopyBuffer(client, target, make([]byte, 1020))
	}()

	<-done
	_ = client.Close()
	_ = target.Close()
	<-done
	slog.Debug("socks5 relay done", "host", host, "port", port)
}

func sendReply(w io.Writer, rep byte) {
	w.Write([]byte{0x05, rep, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})
}
