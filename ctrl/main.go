package main

import (
	"embed"
	"flag"
	"io/fs"
	"log/slog"
	"net/http"
	"os"

	"github.com/manx98/esp32-ctrl/api"
	"github.com/manx98/esp32-ctrl/device"
)

//go:embed static
var staticFiles embed.FS

func main() {
	port  := flag.String("port",   "/dev/ttyACM0", "Serial port (e.g. /dev/ttyACM0 or COM3)")
	baud  := flag.Int("baud",    115200,          "Baud rate")
	addr  := flag.String("addr",  ":8080",         "HTTP listen address")
	debug := flag.Bool("debug",  false,            "Enable debug logging")
	flag.Parse()

	level := slog.LevelInfo
	if *debug {
		level = slog.LevelDebug
	}
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level})))

	// Open device (non-fatal: the web UI still loads even if the device is absent)
	dev := device.New(*port, *baud)
	if err := dev.Open(); err != nil {
		slog.Warn("could not open serial port; UI will show 'not connected'",
			"port", *port, "err", err)
	}
	defer dev.Close()

	// HTTP mux
	mux := http.NewServeMux()

	// API routes
	a := api.New(dev)
	a.Register(mux)

	// Static files (embedded)
	staticFS, _ := fs.Sub(staticFiles, "static")
	mux.Handle("/", http.FileServer(http.FS(staticFS)))

	handler := api.WithCORS(api.WithLogging(mux))

	slog.Info("starting", "addr", *addr, "port", *port)
	if err := http.ListenAndServe(*addr, handler); err != nil {
		slog.Error("server error", "err", err)
		os.Exit(1)
	}
}
