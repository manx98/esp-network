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

	"github.com/manx98/esp32-ctrl/api"
	"github.com/manx98/esp32-ctrl/device"
	"github.com/manx98/esp32-ctrl/socks5"
)

//go:embed static
var staticFiles embed.FS

func main() {
	port := flag.String("port", "/dev/ttyACM0", "Serial port (e.g. /dev/ttyACM0 or COM3)")
	baud := flag.Int("baud", 115200, "Baud rate")
	addr := flag.String("addr", ":8080", "HTTP listen address")
	socks5Addr := flag.String("socks5", ":1080", "SOCKS5 proxy listen address (empty to disable)")
	proxyAddr := flag.String("proxy", "", "External proxy server address (e.g. localhost:11080)")
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
		var proxy *socks5.Server
		if *proxyAddr != "" {
			slog.Info("using external proxy", "addr", *proxyAddr)
			proxyHost, proxyPortStr, err := net.SplitHostPort(*proxyAddr)
			var proxyPort int
			if err == nil {
				proxyPort, err = strconv.Atoi(proxyPortStr)
			}
			if err != nil {
				slog.Warn("could not split proxy address", "addr", *proxyAddr, "err", err)
				os.Exit(1)
			}
			proxy = socks5.NewWithProxy(dev, proxyHost, uint16(proxyPort))
		} else {
			slog.Info("using ESP32 direct connection")
			proxy = socks5.New(dev)
		}
		go func() {
			if err := proxy.ListenAndServe(*socks5Addr); err != nil {
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

	slog.Info("starting", "addr", *addr, "port", *port)
	if err := http.ListenAndServe(*addr, handler); err != nil {
		slog.Error("server error", "err", err)
		os.Exit(1)
	}
}
