// TLS test servers for tests/http_client_tls_test.sh: makes its own CA (written to the path given),
//   8441 localhost (geçerli)        8442 IP SAN 127.0.0.1 (geçerli)   8443 yanlış ad (other.example)
//   8444 süresi dolmuş              8445 kendinden imzalı (CA dışı)   8446 en çok TLS 1.1
//   8447 DNS SAN "127.0.0.1" (IP SAN yok)   8448 geçerli, gelen SNI'yi gövdede bildirir
// Gövde: "ok sni=<istemcinin gönderdiği ad>"
package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"fmt"
	"math/big"
	"net"
	"net/http"
	"os"
	"time"
)

func leaf(ca *x509.Certificate, caKey *ecdsa.PrivateKey, dns []string, ips []net.IP, nb, na time.Time, self bool) tls.Certificate {
	k, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	t := &x509.Certificate{SerialNumber: big.NewInt(time.Now().UnixNano()), Subject: pkix.Name{CommonName: "leaf"},
		DNSNames: dns, IPAddresses: ips, NotBefore: nb, NotAfter: na,
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
	parent, pk := ca, caKey
	if self {
		parent, pk = t, k
	}
	der, err := x509.CreateCertificate(rand.Reader, t, parent, &k.PublicKey, pk)
	if err != nil {
		panic(err)
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: k}
}

func main() {
	caKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	caT := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "LOOK test CA"}, IsCA: true,
		BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign, NotBefore: time.Now().Add(-time.Hour), NotAfter: time.Now().Add(24 * time.Hour)}
	caDER, _ := x509.CreateCertificate(rand.Reader, caT, caT, &caKey.PublicKey, caKey)
	ca, _ := x509.ParseCertificate(caDER)
	os.WriteFile(os.Args[1], pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: caDER}), 0644)
	now := time.Now()
	ok := func(c tls.Certificate) tls.Certificate { return c }
	type srv struct {
		port int
		cert tls.Certificate
		max  uint16
	}
	lh := []string{"localhost"}
	ip := []net.IP{net.ParseIP("127.0.0.1")}
	list := []srv{
		{8441, ok(leaf(ca, caKey, lh, nil, now.Add(-time.Hour), now.Add(time.Hour), false)), 0},
		{8442, leaf(ca, caKey, nil, ip, now.Add(-time.Hour), now.Add(time.Hour), false), 0},
		{8443, leaf(ca, caKey, []string{"other.example"}, nil, now.Add(-time.Hour), now.Add(time.Hour), false), 0},
		{8444, leaf(ca, caKey, lh, nil, now.Add(-48*time.Hour), now.Add(-24*time.Hour), false), 0},
		{8445, leaf(ca, caKey, lh, nil, now.Add(-time.Hour), now.Add(time.Hour), true), 0},
		{8446, leaf(ca, caKey, lh, nil, now.Add(-time.Hour), now.Add(time.Hour), false), tls.VersionTLS11},
		{8447, leaf(ca, caKey, []string{"127.0.0.1"}, nil, now.Add(-time.Hour), now.Add(time.Hour), false), 0},
		{8448, leaf(ca, caKey, lh, ip, now.Add(-time.Hour), now.Add(time.Hour), false), 0},
	}
	for _, s := range list {
		s := s
		cfg := &tls.Config{Certificates: []tls.Certificate{s.cert}, MinVersion: tls.VersionTLS10}
		if s.max != 0 {
			cfg.MaxVersion = s.max
		}
		mux := http.NewServeMux()
		mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprintf(w, "ok sni=%s", r.TLS.ServerName)
		})
		hs := &http.Server{Addr: fmt.Sprintf("127.0.0.1:%d", s.port), Handler: mux, TLSConfig: cfg}
		go hs.ListenAndServeTLS("", "")
	}
	fmt.Println("ready")
	select {}
}
