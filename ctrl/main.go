package main

import (
	"embed"
	"flag"
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

func main() {
	port := flag.String("port", "/dev/ttyACM0", "Serial port (e.g. /dev/ttyACM0 or COM3)")
	baud := flag.Int("baud", 115200, "Baud rate")
	addr := flag.String("addr", ":8080", "HTTP listen address")
	socks5Addr := flag.String("socks5", ":1080", "Proxy listen address for SOCKS5+HTTP/HTTPS (empty to disable)")
	proxyAddr := flag.String("proxy", "", "External proxy server address (e.g. localhost:11080)")
	auth := flag.String("auth", "", "HTTP Basic Auth credentials in user:password format (empty to disable)")
	debug := flag.Bool("debug", false, "Enable debug logging")
	flag.Parse()

	level := slog.LevelInfo
	if *debug {
		level = slog.LevelDebug
	}
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level})))

	dev := device.New(*port, *baud)
	if err := dev.Open(); err != nil {
		slog.Warn("could not open serial port; UI will show 'not connected'",
			"port", *port, "err", err)
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

	var handler http.Handler = api.WithCORS(api.WithLogging(mux))
	if *auth != "" {
		parts := strings.SplitN(*auth, ":", 2)
		if len(parts) != 2 || parts[0] == "" || parts[1] == "" {
			slog.Error("--auth must be in user:password format")
			os.Exit(1)
		}
		sa := api.NewSessionAuth(parts[0], parts[1])
		mux.HandleFunc("GET /login", sa.HandleLogin)
		mux.HandleFunc("POST /login", sa.HandleLogin)
		mux.HandleFunc("POST /api/logout", sa.HandleLogout)
		handler = api.WithCORS(api.WithLogging(sa.Middleware(mux)))
		slog.Info("auth enabled", "user", parts[0])
	}

	slog.Info("starting", "addr", *addr, "port", *port)
	if err := http.ListenAndServe(*addr, handler); err != nil {
		slog.Error("server error", "err", err)
		os.Exit(1)
	}
}
