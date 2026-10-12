0.  install requirements & setup mariadb-server

1.  create an adapter for your game & run on the server (mainly plugins through game api)

2. 	cd nektumshield

3. 	go mod init nektumshield

4. 	go get github.com/go-sql-driver/mysql

5.  if u wanna compile a binary: go build -o nektumshield .

6.  before using binary do: chmod +x ./nektumshield

7.  run the anticheat with: ./nektumshield (or) without binary simply: go run .