// Package api exposes the device control REST API.
package api

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/manx98/esp32-ctrl/device"
	"github.com/manx98/esp32-ctrl/proto"
)

// API wires the HTTP mux to the device.
type API struct {
	dev *device.Device

	// manuallyDisconnected is set when the user explicitly calls disconnect,
	// and cleared when they call connect. It persists across page reloads so
	// the UI can distinguish "user disconnected" from "never connected".
	mu                   sync.Mutex
	manuallyDisconnected bool
}

// New creates an API backed by dev.
func New(dev *device.Device) *API {
	return &API{dev: dev}
}

// Register mounts all routes onto mux and serves the embedded static UI.
func (a *API) Register(mux *http.ServeMux) {
	// System
	mux.HandleFunc("GET /api/ping", a.handlePing)
	mux.HandleFunc("GET /api/info", a.handleGetDevInfo)
	mux.HandleFunc("POST /api/reset", a.handleReset)
	mux.HandleFunc("GET /api/status", a.handleDeviceStatus)

	// WiFi
	mux.HandleFunc("POST /api/wifi/config", a.handleWifiSetConfig)
	mux.HandleFunc("GET /api/wifi/config", a.handleWifiGetConfig)
	mux.HandleFunc("POST /api/wifi/connect", a.handleWifiConnect)
	mux.HandleFunc("POST /api/wifi/disconnect", a.handleWifiDisconnect)
	mux.HandleFunc("GET /api/wifi/status", a.handleWifiGetStatus)
	mux.HandleFunc("GET /api/wifi/scan", a.handleWifiScan)

	// Hostname
	mux.HandleFunc("POST /api/wifi/hostname", a.handleWifiSetHostname)
	mux.HandleFunc("GET /api/wifi/hostname", a.handleWifiGetHostname)
}

// ── helpers ──────────────────────────────────────────────────────────────────

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

type errResp struct {
	Error string `json:"error"`
}

func writeErr(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, errResp{Error: msg})
}

// send wraps device.Send with a per-request context (30 s max).
func (a *API) send(r *http.Request, cmd proto.Cmd, payload []byte) (proto.Frame, error) {
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	return a.dev.Send(ctx, cmd, payload)
}

func checkStatus(f proto.Frame) error {
	if f.Status != proto.StatusOK {
		return fmt.Errorf("device returned status %s", f.Status)
	}
	return nil
}

// ── System handlers ──────────────────────────────────────────────────────────

func (a *API) handleDeviceStatus(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"connected": a.dev.IsConnected(),
	})
}

func (a *API) handlePing(w http.ResponseWriter, r *http.Request) {
	f, err := a.send(r, proto.CmdPing, nil)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"pong": string(f.Payload)})
}

func (a *API) handleGetDevInfo(w http.ResponseWriter, r *http.Request) {
	f, err := a.send(r, proto.CmdGetDevInfo, nil)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}

	// Payload: [fw_ver_len:1][fw_ver:N][free_heap:4LE][total_heap:4LE]
	//          [min_heap:4LE][cpu_load:1][uptime_s:4LE][task_count:2LE]
	p := f.Payload
	if len(p) < 1 {
		writeErr(w, http.StatusBadGateway, "short payload")
		return
	}
	verLen := int(p[0])
	p = p[1:]
	if len(p) < verLen+15 { // 3×4 + 1 + 4 + 2 = 15
		writeErr(w, http.StatusBadGateway, "truncated payload")
		return
	}
	fwVer := string(p[:verLen])
	p = p[verLen:]

	le32 := func(b []byte) uint32 {
		return uint32(b[0]) | uint32(b[1])<<8 | uint32(b[2])<<16 | uint32(b[3])<<24
	}
	freeHeap := le32(p[0:4])
	totalHeap := le32(p[4:8])
	minHeap := le32(p[8:12])
	cpuLoad := p[12]
	uptimeS := le32(p[13:17])
	taskCount := uint16(p[17]) | uint16(p[18])<<8

	heapUsedPct := uint32(0)
	if totalHeap > 0 {
		heapUsedPct = (totalHeap - freeHeap) * 100 / totalHeap
	}

	writeJSON(w, http.StatusOK, map[string]any{
		"fw_version":    fwVer,
		"free_heap":     freeHeap,
		"total_heap":    totalHeap,
		"min_free_heap": minHeap,
		"heap_used_pct": heapUsedPct,
		"cpu_load":      cpuLoad,
		"uptime_s":      uptimeS,
		"task_count":    taskCount,
		"tx_bytes":      binary.LittleEndian.Uint64(p[19:27]),
		"rx_bytes":      binary.LittleEndian.Uint64(p[27:35]),
	})
}

func (a *API) handleReset(w http.ResponseWriter, r *http.Request) {
	_, err := a.send(r, proto.CmdReset, nil)
	// Device resets after sending response; a timeout here is acceptable.
	if err != nil && err != device.ErrTimeout {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// ── WiFi handlers ─────────────────────────────────────────────────────────────

// wifiConfigReq is the JSON body for POST /api/wifi/config.
type wifiConfigReq struct {
	SSID     string `json:"ssid"`
	Password string `json:"password"`
}

func (a *API) handleWifiSetConfig(w http.ResponseWriter, r *http.Request) {
	var req wifiConfigReq
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if len(req.SSID) == 0 || len(req.SSID) > 32 {
		writeErr(w, http.StatusBadRequest, "ssid must be 1–32 bytes")
		return
	}
	if len(req.Password) > 64 {
		writeErr(w, http.StatusBadRequest, "password must be ≤ 64 bytes")
		return
	}

	// Build payload: [ssid_len:1][ssid][pass_len:1][pass]
	ssid := []byte(req.SSID)
	pass := []byte(req.Password)
	payload := make([]byte, 0, 2+len(ssid)+len(pass))
	payload = append(payload, byte(len(ssid)))
	payload = append(payload, ssid...)
	payload = append(payload, byte(len(pass)))
	payload = append(payload, pass...)

	f, err := a.send(r, proto.CmdWifiSetConfig, payload)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}
	// New config saved → treat as "disconnected" so the Connect button appears.
	a.mu.Lock()
	a.manuallyDisconnected = true
	a.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (a *API) handleWifiGetConfig(w http.ResponseWriter, r *http.Request) {
	f, err := a.send(r, proto.CmdWifiGetConfig, nil)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}

	// Payload: [ssid_len:1][ssid][pass_len:1][pass_masked]
	p := f.Payload
	if len(p) < 1 {
		writeErr(w, http.StatusBadGateway, "short payload")
		return
	}
	ssidLen := int(p[0])
	if len(p) < 1+ssidLen+1 {
		writeErr(w, http.StatusBadGateway, "truncated payload")
		return
	}
	ssid := string(p[1 : 1+ssidLen])
	passLen := int(p[1+ssidLen])
	passMask := fmt.Sprintf("%s", p[2+ssidLen:2+ssidLen+passLen])

	writeJSON(w, http.StatusOK, map[string]string{
		"ssid":     ssid,
		"password": passMask,
	})
}

func (a *API) handleWifiConnect(w http.ResponseWriter, r *http.Request) {
	f, err := a.send(r, proto.CmdWifiConnect, nil)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}
	a.mu.Lock()
	a.manuallyDisconnected = false
	a.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (a *API) handleWifiDisconnect(w http.ResponseWriter, r *http.Request) {
	f, err := a.send(r, proto.CmdWifiDisconnect, nil)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}
	a.mu.Lock()
	a.manuallyDisconnected = true
	a.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

// wifiStateNames maps wifi_mgr_state_t values to strings.
var wifiStateNames = map[byte]string{
	0: "disconnected",
	1: "connecting",
	2: "connected",
	3: "failed",
}

func (a *API) handleWifiGetStatus(w http.ResponseWriter, r *http.Request) {
	f, err := a.send(r, proto.CmdWifiGetStatus, nil)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}
	// Payload: [state:1][ip:4][rssi:1]
	if len(f.Payload) < 6 {
		writeErr(w, http.StatusBadGateway, "short status payload")
		return
	}
	state := f.Payload[0]
	ip := net.IPv4(f.Payload[1], f.Payload[2], f.Payload[3], f.Payload[4]).String()
	rssi := int(int8(f.Payload[5]))
	stateName, ok := wifiStateNames[state]
	if !ok {
		stateName = "unknown"
	}
	a.mu.Lock()
	manuallyDisconnected := a.manuallyDisconnected
	a.mu.Unlock()

	writeJSON(w, http.StatusOK, map[string]any{
		"state":                 state,
		"state_name":            stateName,
		"ip":                    ip,
		"rssi":                  rssi,
		"manually_disconnected": manuallyDisconnected,
	})
}

// APInfo is a scanned access point.
type APInfo struct {
	SSID     string `json:"ssid"`
	RSSI     int    `json:"rssi"`
	AuthMode int    `json:"authmode"`
}

func (a *API) handleWifiScan(w http.ResponseWriter, r *http.Request) {
	// Scan can take up to 10 s on the device side; extend timeout.
	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	defer cancel()

	f, err := a.dev.Send(ctx, proto.CmdWifiScan, nil)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}

	// Payload: [count:1]([ssid_len:1][ssid][rssi:1][auth:1])*
	p := f.Payload
	if len(p) < 1 {
		writeJSON(w, http.StatusOK, map[string]any{"aps": []APInfo{}})
		return
	}
	count := int(p[0])
	p = p[1:]

	aps := make([]APInfo, 0, count)
	for i := 0; i < count && len(p) >= 1; i++ {
		ssidLen := int(p[0])
		p = p[1:]
		if len(p) < ssidLen+2 {
			break
		}
		ap := APInfo{
			SSID:     string(p[:ssidLen]),
			RSSI:     int(int8(p[ssidLen])),
			AuthMode: int(p[ssidLen+1]),
		}
		aps = append(aps, ap)
		p = p[ssidLen+2:]
	}

	writeJSON(w, http.StatusOK, map[string]any{"aps": aps})
}

// ── Hostname handlers ─────────────────────────────────────────────────────────

type hostnameReq struct {
	Hostname string `json:"hostname"`
}

func (a *API) handleWifiSetHostname(w http.ResponseWriter, r *http.Request) {
	var req hostnameReq
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid JSON: "+err.Error())
		return
	}
	if len(req.Hostname) == 0 || len(req.Hostname) > 32 {
		writeErr(w, http.StatusBadRequest, "hostname must be 1–32 bytes")
		return
	}

	h := []byte(req.Hostname)
	payload := make([]byte, 1+len(h))
	payload[0] = byte(len(h))
	copy(payload[1:], h)

	f, err := a.send(r, proto.CmdWifiSetHostname, payload)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]bool{"ok": true})
}

func (a *API) handleWifiGetHostname(w http.ResponseWriter, r *http.Request) {
	f, err := a.send(r, proto.CmdWifiGetHostname, nil)
	if err != nil {
		writeErr(w, http.StatusGatewayTimeout, err.Error())
		return
	}
	if err := checkStatus(f); err != nil {
		writeErr(w, http.StatusBadGateway, err.Error())
		return
	}

	p := f.Payload
	if len(p) < 1 {
		writeErr(w, http.StatusBadGateway, "short payload")
		return
	}
	hlen := int(p[0])
	if len(p) < 1+hlen {
		writeErr(w, http.StatusBadGateway, "truncated payload")
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"hostname": string(p[1 : 1+hlen])})
}

// ── Middleware ────────────────────────────────────────────────────────────────

// WithLogging wraps an http.Handler with request logging.
func WithLogging(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		h.ServeHTTP(w, r)
		slog.Info("http", "method", r.Method, "path", r.URL.Path,
			"duration", time.Since(start).String())
	})
}

// WithCORS adds permissive CORS headers (useful during development).
func WithCORS(h http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		h.ServeHTTP(w, r)
	})
}
