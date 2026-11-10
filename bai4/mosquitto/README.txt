Tạo file tài khoản (băm, không commit) trước khi chạy compose:

  docker run --rm -it -v "$PWD/mosquitto:/mosquitto/config" eclipse-mosquitto:2 \
      mosquitto_passwd -c /mosquitto/config/passwords nhom01

Chạy:      docker compose up -d
Kiểm tra:  mosquitto_sub -h <IP_PC> -u nhom01 -P <mat_khau> -t "nhom01/node01/telemetry" -v | tee log_10min.txt
Ngắt thử:  docker compose stop broker ; sleep 30 ; docker compose start broker
Gửi lệnh:  mosquitto_pub -h <IP_PC> -u nhom01 -P <mat_khau> -t "nhom01/node01/cmd" -m "ON"
