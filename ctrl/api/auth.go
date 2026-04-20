package api

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/hex"
	"net/http"
	"time"
)

const sessionCookie = "esp32_session"

// SessionAuth implements cookie-based single-user authentication.
type SessionAuth struct {
	user  string
	pass  string
	token string // random token generated at startup, invalidated on logout
}

// NewSessionAuth creates a SessionAuth with the given credentials.
func NewSessionAuth(user, pass string) *SessionAuth {
	return &SessionAuth{user: user, pass: pass, token: newToken()}
}

func newToken() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}

// Middleware rejects unauthenticated requests with a redirect to /login,
// except for /login and /login.html (to avoid redirect loops).
func (sa *SessionAuth) Middleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		p := r.URL.Path
		if p == "/login" || p == "/login.html" {
			next.ServeHTTP(w, r)
			return
		}
		cookie, err := r.Cookie(sessionCookie)
		if err != nil || subtle.ConstantTimeCompare([]byte(cookie.Value), []byte(sa.token)) != 1 {
			if isAPIPath(p) {
				http.Error(w, "Unauthorized", http.StatusUnauthorized)
				return
			}
			http.Redirect(w, r, "/login", http.StatusFound)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func isAPIPath(path string) bool {
	return len(path) >= 4 && path[:4] == "/api"
}

// HandleLogin handles GET (show form) and POST (validate credentials).
func (sa *SessionAuth) HandleLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Redirect(w, r, "/login.html", http.StatusFound)
		return
	}
	if err := r.ParseForm(); err != nil {
		http.Error(w, "bad request", http.StatusBadRequest)
		return
	}
	u := r.FormValue("username")
	p := r.FormValue("password")
	if subtle.ConstantTimeCompare([]byte(u), []byte(sa.user)) != 1 ||
		subtle.ConstantTimeCompare([]byte(p), []byte(sa.pass)) != 1 {
		http.Redirect(w, r, "/login.html?error=1", http.StatusFound)
		return
	}
	http.SetCookie(w, &http.Cookie{
		Name:     sessionCookie,
		Value:    sa.token,
		Path:     "/",
		HttpOnly: true,
		SameSite: http.SameSiteStrictMode,
	})
	http.Redirect(w, r, "/", http.StatusFound)
}

// HandleLogout clears the session cookie and rotates the token so existing
// sessions are invalidated, then redirects to /login.
func (sa *SessionAuth) HandleLogout(w http.ResponseWriter, r *http.Request) {
	sa.token = newToken() // invalidate all existing sessions
	http.SetCookie(w, &http.Cookie{
		Name:     sessionCookie,
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		Expires:  time.Unix(0, 0),
		MaxAge:   -1,
	})
	http.Redirect(w, r, "/login", http.StatusFound)
}
