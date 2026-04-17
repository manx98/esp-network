package main

import (
	"embed"
	"flag"
	"fmt"
	"io/fs"
	"log/slog"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"

	"github.com/manx98/esp32-ctrl/api"
	"github.com/manx98/esp32-ctrl/device"
	"github.com/manx98/esp32-ctrl/proxy"
	"github.com/manx98/esp32-ctrl/socks5"
)

//go:embed static
var staticFiles embed.FS

func parseHex(s string) (uint16, error) {
	s = strings.TrimPrefix(strings.ToLower(s), "0x")
	v, err := strconv.ParseUint(s, 16, 16)
	if err != nil {
		return 0, fmt.Errorf("invalid hex value %q: %w", s, err)
	}
	return uint16(v), nil
}

func main() {
	vid := flag.String("vid", fmt.Sprintf("0x%04X", device.DefaultVID),
		"USB Vendor ID of the ESP32 HID device (e.g. 0x303A)")
	pid := flag.String("pid", fmt.Sprintf("0x%04X", device.DefaultPID),
		"USB Product ID of the ESP32 HID device (e.g. 0x4004)")
	addr := flag.String("addr", ":8080", "HTTP listen address")
	socks5Addr := flag.String("socks5", ":1080", "Proxy listen address for SOCKS5+HTTP/HTTPS (empty to disable)")
	proxyAddr := flag.String("proxy", "", "External proxy server address (e.g. localhost:11080)")
	debug := flag.Bool("debug", false, "Enable debug logging")
	flag.Parse()

	level := slog.LevelInfo
	if *debug {
		level = slog.LevelDebug
	}
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level})))

	vidN, err := parseHex(*vid)
	if err != nil {
		slog.Error("invalid --vid", "err", err)
		os.Exit(1)
	}
	pidN, err := parseHex(*pid)
	if err != nil {
		slog.Error("invalid --pid", "err", err)
		os.Exit(1)
	}

	dev := device.New(vidN, pidN)
	if err := dev.Open(); err != nil {
		slog.Error("failed to initialise HID", "err", err)
		os.Exit(1)
	}
	defer dev.Close()

	if *socks5Addr != "" {
		if *proxyAddr == "" {
			slog.Error("--proxy is required when --socks5 is enabled")
			os.Exit(1)
		}
		proxyHost, proxyPortStr, err := net.SplitHostPort(*proxyAddr)
		var proxyPort int
		if err == nil {
			proxyPort, err = strconv.Atoi(proxyPortStr)
		}
		if err != nil {
			slog.Error("invalid --proxy address", "addr", *proxyAddr, "err", err)
			os.Exit(1)
		}
		ps := proxy.New(proxyHost, uint16(proxyPort), dev)
		defer ps.Close()
		slog.Info("proxy relay target", "addr", *proxyAddr)
		go func() {
			if err := socks5.New(ps).ListenAndServe(*socks5Addr); err != nil {
				slog.Error("socks5 proxy error", "err", err)
			}
		}()
	}

	mux := http.NewServeMux()

	a := api.New(dev)
	a.Register(mux)

	staticFS, _ := fs.Sub(staticFiles, "static")
	mux.Handle("/", http.FileServer(http.FS(staticFS)))

	handler := api.WithCORS(api.WithLogging(mux))

	slog.Info("starting", "addr", *addr, "vid", *vid, "pid", *pid)
	if err := http.ListenAndServe(*addr, handler); err != nil {
		slog.Error("server error", "err", err)
		os.Exit(1)
	}
}
