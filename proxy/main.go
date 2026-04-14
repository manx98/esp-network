package main

import (
	"context"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"os"
	"sync"
	"time"
)

const (
	CmdProxyConnect byte = iota
	CmdProxyData
	CmdProxyClose
	CmdProxyPing

	heartbeatInterval = 10 * time.Second
)

const (
	StatusOK byte = iota
	StatusError
	StatusRejected
)

type ProxyServer struct {
	conn       net.Conn
	mu         sync.Mutex
	conns      map[uint32]net.Conn
	wLock      chan struct{}
	nextConnID uint32
	ctx        context.Context
	cause      context.CancelCauseFunc
}

func New(conn net.Conn) *ProxyServer {
	server := &ProxyServer{
		conn:  conn,
		conns: make(map[uint32]net.Conn),
		wLock: make(chan struct{}, 1),
	}
	server.ctx, server.cause = context.WithCancelCause(context.Background())
	return server
}

func (s *ProxyServer) cleanup() {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, conn := range s.conns {
		_ = conn.Close()
	}
}

func (s *ProxyServer) Run() {
	slog.Info("client connected", "addr", s.conn.RemoteAddr())

	heartbeat := time.NewTicker(heartbeatInterval)
	defer s.cleanup()
	defer heartbeat.Stop()
	go func() {
		for {
			select {
			case <-heartbeat.C:
				s.sendPing()
			case <-s.ctx.Done():
			}
		}
	}()

	buf := make([]byte, 7)
	for s.ctx.Err() == nil {
		select {
		case <-heartbeat.C:
			s.sendPing()
		default:
		}
		_, err := io.ReadFull(s.conn, buf)
		if err != nil {
			slog.Info("client read error", "err", err)
			s.cause(err)
			return
		}
		connId := binary.LittleEndian.Uint32(buf[1:5])
		payloadLen := binary.LittleEndian.Uint16(buf[5:7])
		payload := make([]byte, payloadLen)
		_, err = io.ReadFull(s.conn, payload)
		if err != nil {
			slog.Info("client read error", "err", err)
			s.cause(err)
			return
		}
		s.handleClientData(buf[0], connId, payload)
	}
}

func (s *ProxyServer) writeCmd(cmd byte, connID uint32, data []byte) {
	select {
	case s.wLock <- struct{}{}:
		defer func() { <-s.wLock }()
	case <-s.ctx.Done():
		return
	}
	n := len(data)
	frame := make([]byte, 7+n)
	frame[0] = cmd
	binary.LittleEndian.PutUint32(frame[1:], connID)
	binary.LittleEndian.PutUint16(frame[5:], uint16(n))
	copy(frame[7:], data)
	_, err := s.conn.Write(frame)
	if err != nil {
		s.cause(err)
	}
}

func (s *ProxyServer) sendPing() {
	s.writeCmd(CmdProxyPing, 0, nil)
}

func (s *ProxyServer) handleClientData(cmd byte, connID uint32, payload []byte) {
	switch cmd {
	case CmdProxyConnect:
		slog.Info("proxy: connect command", "connID", connID, "payloadLen", len(payload))
		s.handleProxyConnect(connID, payload)
	case CmdProxyData:
		slog.Info("proxy: data command", "connID", connID, "dataLen", len(payload))
		s.handleProxyData(connID, payload)
	case CmdProxyClose:
		slog.Info("proxy: close command", "connID", connID)
		s.handleProxyClose(connID)
	case CmdProxyPing:
		// 心跳响应
		slog.Info("proxy: ping command")
		s.writeCmd(CmdProxyPing, 0, nil)
	default:
		slog.Warn("proxy: unknown cmd", "cmd", cmd)
	}
}

func (s *ProxyServer) handleProxyConnect(connID uint32, payload []byte) {
	if len(payload) < 3 {
		s.cause(errors.New("invalid play"))
		return
	}

	hostLen := int(payload[0])
	if len(payload) != 1+hostLen+2 {
		s.writeCmd(CmdProxyConnect, connID, []byte{StatusError})
		return
	}

	host := string(payload[1 : 1+hostLen])
	port := binary.LittleEndian.Uint16(payload[1+hostLen:])

	slog.Info("proxy connect", "connID", connID, "host", host, "port", port)
	target, err := net.DialTimeout("tcp", fmt.Sprintf("%s:%d", host, port), 10*time.Second)
	if err != nil {
		slog.Warn("dial failed", "host", host, "port", port, "err", err)
		s.writeCmd(CmdProxyConnect, connID, []byte{StatusRejected})
		return
	}

	s.mu.Lock()
	s.conns[connID] = target
	s.mu.Unlock()

	s.writeCmd(CmdProxyConnect, connID, []byte{StatusOK})
	go s.relayData(connID, target)
}

func (s *ProxyServer) handleProxyData(connID uint32, payload []byte) {
	s.mu.Lock()
	pc := s.conns[connID]
	s.mu.Unlock()

	if pc == nil {
		slog.Warn("proxy: data for unknown connection", "connID", connID)
		return
	}

	n, err := pc.Write(payload)
	if err != nil {
		slog.Warn("proxy: write to target failed", "connID", connID, "err", err)
		return
	}
	slog.Info("proxy: wrote data to target", "connID", connID, "bytes", n)
	return
}

func (s *ProxyServer) handleProxyClose(connID uint32) {
	s.mu.Lock()
	pc := s.conns[connID]
	delete(s.conns, connID)
	s.mu.Unlock()
	if pc == nil {
		slog.Warn("proxy: close for unknown connection", "connID", connID)
		return
	}
	_ = pc.Close()
}

func (s *ProxyServer) relayData(connId uint32, conn net.Conn) {
	var err error
	defer func() {
		s.mu.Lock()
		delete(s.conns, connId)
		s.mu.Unlock()
		s.writeCmd(CmdProxyClose, connId, nil)
		_ = conn.Close()
	}()

	buf := make([]byte, 4096)
	var n int
	for s.ctx.Err() == nil {
		n, err = conn.Read(buf)
		if n > 0 {
			s.writeCmd(CmdProxyData, connId, buf[:n])
		}
		if err != nil {
			break
		}
	}
}

func main() {
	addr := flag.String("addr", ":11080", "Proxy server listen address")
	flag.Parse()

	level := slog.LevelInfo
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level})))

	ln, err := net.Listen("tcp", *addr)
	if err != nil {
		slog.Error("proxy: listen failed", "err", err)
		os.Exit(1)
	}
	slog.Info("starting proxy server", "addr", *addr)
	for {
		accept, err := ln.Accept()
		if err != nil {
			slog.Error("proxy: accept failed", "err", err)
			os.Exit(1)
		}
		go New(accept).Run()
	}
}
