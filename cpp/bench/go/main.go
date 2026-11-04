// Go counterpart of bench.lk / dbapp.lk: net/http, production defaults.
// Without DB_HOST: GET / → {"ok":true,"msg":"hello"}. With DB_HOST: GET / → the row of users id=42.
package main

import (
	"database/sql"
	"encoding/json"
	"log"
	"net/http"
	"os"
	"time"

	_ "github.com/go-sql-driver/mysql"
)

type user struct {
	ID    int    `json:"id"`
	Name  string `json:"name"`
	Email string `json:"email"`
}

func main() {
	if host := os.Getenv("DB_HOST"); host != "" {
		db, err := sql.Open("mysql", "root:"+os.Getenv("DB_PASS")+"@tcp("+host+":3306)/bench")
		if err != nil {
			log.Fatal(err)
		}
		db.SetMaxOpenConns(16)
		db.SetMaxIdleConns(16)
		for i := 0; db.Ping() != nil; i++ { // the database may still be starting
			if i == 60 {
				log.Fatal("MySQL never became reachable")
			}
			time.Sleep(500 * time.Millisecond)
		}
		http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			rows, err := db.Query("SELECT id, name, email FROM users WHERE id = ?", 42)
			if err != nil {
				http.Error(w, err.Error(), 500)
				return
			}
			defer rows.Close()
			out := []user{}
			for rows.Next() {
				var u user
				if err := rows.Scan(&u.ID, &u.Name, &u.Email); err != nil {
					http.Error(w, err.Error(), 500)
					return
				}
				out = append(out, u)
			}
			w.Header().Set("Content-Type", "application/json")
			json.NewEncoder(w).Encode(out)
		})
	} else {
		http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "application/json")
			json.NewEncoder(w).Encode(map[string]any{"ok": true, "msg": "hello"})
		})
	}
	log.Fatal(http.ListenAndServe(":8080", nil))
}
