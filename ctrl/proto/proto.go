// Package proto implements the ESP32 binary serial protocol.
//
// Frame format:
//
//	[AA][55][LEN_H][LEN_L][SEQ][CMD][PAYLOAD...][CRC_H][CRC_L]
//
// LEN = 2 + len(PAYLOAD)  (counts SEQ + CMD + PAYLOAD)
// CRC = CRC16-CCITT (poly 0x1021, init 0xFFFF) over all bytes before CRC.
//
// Response frames have CMD | 0x80, PAYLOAD[0] is the status byte.
package proto

// Magic bytes
const (
	Magic0   byte = 0xAA
	Magic1   byte = 0x55
	RespFlag byte = 0x80
)

const MaxPayload = 1024

// Cmd is a protocol command byte.
type Cmd byte

// Commands — ranges are reserved for future use:
//
//	0x01–0x0F  System
//	0x10–0x1F  WiFi
//	0x20–0x2F  Network (reserved)
//	0x30–0x3F  OTA     (reserved)
//	0x40–0x7E  User-defined
const (
	CmdPing       Cmd = 0x01
	CmdGetDevInfo Cmd = 0x02
	CmdReset      Cmd = 0x03
	CmdLedSet     Cmd = 0x04 // [enabled:1]  0=off 1=on
	CmdLedGet     Cmd = 0x05 // → [enabled:1]
	CmdWifiSetConfig  Cmd = 0x10
	CmdWifiGetConfig  Cmd = 0x11
	CmdWifiConnect      Cmd = 0x12
	CmdWifiDisconnect   Cmd = 0x13
	CmdWifiGetStatus    Cmd = 0x14
	CmdWifiScan         Cmd = 0x15
	CmdWifiSetHostname  Cmd = 0x16 // [hostname_len:1][hostname]
	CmdWifiGetHostname  Cmd = 0x17 // → [hostname_len:1][hostname]

	// Proxy relay — single long-lived TCP connection to the proxy server (0x20–0x2F).
	// The ctrl side handles proxy-protocol framing; ESP32 is a transparent relay.
	CmdProxyConnect    Cmd = 0x20 // [host_len:1][host:N][port_hi:1][port_lo:1] → OK/ERROR
	CmdProxySend       Cmd = 0x21 // [data...] (fire-and-forget, no response)
	CmdProxyDisconnect Cmd = 0x22 // → OK
	CmdProxyGetStatus  Cmd = 0x23 // → [connected:1]

	// Push frames from ESP32 (IsResp=false, unsolicited)
	CmdProxyDataPush   Cmd = 0x40 // [data...] — raw bytes from proxy server
	CmdProxyClosedPush Cmd = 0x41 // (no payload) — relay connection dropped
)

// Status is the first byte of every response payload.
type Status byte

const (
	StatusOK      Status = 0x00
	StatusError   Status = 0x01
	StatusBusy    Status = 0x02
	StatusInvalid Status = 0x03
	StatusTimeout Status = 0x04
)

func (s Status) String() string {
	switch s {
	case StatusOK:
		return "OK"
	case StatusError:
		return "error"
	case StatusBusy:
		return "busy"
	case StatusInvalid:
		return "invalid"
	case StatusTimeout:
		return "timeout"
	default:
		return "unknown"
	}
}

// WiFi connection states (from CMD_WIFI_GET_STATUS response payload[1])
const (
	WifiStateDisconnected = 0
	WifiStateConnecting   = 1
	WifiStateConnected    = 2
	WifiStateFailed       = 3
)

// Frame is a parsed protocol frame.
type Frame struct {
	Seq     byte
	Cmd     Cmd
	IsResp  bool   // true when CMD has RespFlag set
	Status  Status // valid only when IsResp
	Payload []byte // data after the status byte (for responses)
}

// BuildRequest encodes a request frame ready to write to the serial port.
func BuildRequest(seq byte, cmd Cmd, payload []byte) []byte {
	bodyLen := 2 + len(payload) // SEQ + CMD + PAYLOAD
	total := 2 + 2 + bodyLen + 2
	buf := make([]byte, total)

	n := 0
	buf[n] = Magic0
	n++
	buf[n] = Magic1
	n++
	buf[n] = byte(bodyLen >> 8)
	n++
	buf[n] = byte(bodyLen)
	n++
	buf[n] = seq
	n++
	buf[n] = byte(cmd)
	n++
	copy(buf[n:], payload)
	n += len(payload)

	crc := crc16(buf[:n])
	buf[n] = byte(crc >> 8)
	n++
	buf[n] = byte(crc)
	n++

	return buf[:n]
}

// crc16 computes CRC16-CCITT.
func crc16(data []byte) uint16 {
	crc := uint16(0xFFFF)
	for _, b := range data {
		crc ^= uint16(b) << 8
		for i := 0; i < 8; i++ {
			if crc&0x8000 != 0 {
				crc = (crc << 1) ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

// ── Parser ───────────────────────────────────────────────────────────────────

type parseState int

const (
	stMagic0 parseState = iota
	stMagic1
	stLenH
	stLenL
	stBody
	stCrcH
	stCrcL
)

// Parser is a streaming, stateful frame parser. Feed it bytes as they arrive;
// OnFrame is called for every valid, CRC-verified frame.
type Parser struct {
	state    parseState
	bodyLen  uint16
	body     []byte
	bodyRecv int
	crcH     byte
	crcCalc  uint16
	OnFrame  func(Frame)
}

// NewParser creates a parser that calls onFrame for each validated frame.
func NewParser(onFrame func(Frame)) *Parser {
	return &Parser{
		state:   stMagic0,
		crcCalc: 0xFFFF,
		OnFrame: onFrame,
	}
}

func (p *Parser) reset() {
	p.state = stMagic0
	p.crcCalc = 0xFFFF
}

func (p *Parser) updateCRC(b byte) {
	p.crcCalc ^= uint16(b) << 8
	for i := 0; i < 8; i++ {
		if p.crcCalc&0x8000 != 0 {
			p.crcCalc = (p.crcCalc << 1) ^ 0x1021
		} else {
			p.crcCalc <<= 1
		}
	}
}

// Feed processes a chunk of bytes. Safe to call from a single goroutine.
func (p *Parser) Feed(data []byte) {
	for _, b := range data {
		switch p.state {

		case stMagic0:
			if b == Magic0 {
				p.crcCalc = 0xFFFF
				p.updateCRC(b)
				p.state = stMagic1
			}

		case stMagic1:
			p.updateCRC(b)
			if b == Magic1 {
				p.state = stLenH
			} else {
				p.reset()
				if b == Magic0 {
					p.updateCRC(b)
					p.state = stMagic1
				}
			}

		case stLenH:
			p.updateCRC(b)
			p.bodyLen = uint16(b) << 8
			p.state = stLenL

		case stLenL:
			p.updateCRC(b)
			p.bodyLen |= uint16(b)
			if p.bodyLen < 2 || p.bodyLen > 2+MaxPayload {
				p.reset()
			} else {
				p.body = make([]byte, p.bodyLen)
				p.bodyRecv = 0
				p.state = stBody
			}

		case stBody:
			p.updateCRC(b)
			p.body[p.bodyRecv] = b
			p.bodyRecv++
			if p.bodyRecv >= int(p.bodyLen) {
				p.state = stCrcH
			}

		case stCrcH:
			p.crcH = b
			p.state = stCrcL

		case stCrcL:
			crcRecv := uint16(p.crcH)<<8 | uint16(b)
			if crcRecv == p.crcCalc && p.OnFrame != nil {
				cmdByte := p.body[1]
				isResp := cmdByte&byte(RespFlag) != 0
				cmd := Cmd(cmdByte &^ byte(RespFlag))

				f := Frame{
					Seq:    p.body[0],
					Cmd:    cmd,
					IsResp: isResp,
				}
				if isResp && len(p.body) > 2 {
					f.Status = Status(p.body[2])
					f.Payload = append([]byte(nil), p.body[3:]...)
				} else {
					f.Payload = append([]byte(nil), p.body[2:]...)
				}
				p.OnFrame(f)
			}
			p.reset()
		}
	}
}
