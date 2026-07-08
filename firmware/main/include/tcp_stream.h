#ifndef TCP_STREAM_H
#define TCP_STREAM_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * TCP transport for audio streaming.
 *
 * ESP = listener (TCP server). После CONFIGURE открывает listening socket
 * на stream_port и ждёт connect от приёмника. Один активный коннект за раз.
 *
 * Framing (TCP = поток без границ, нужны разделители):
 *   Каждый кадр = [u16 length BE][16-byte pkt_header][payload]
 *   length = 16 + payload_len (≤ 1416, влезает в u16).
 *   Приёмник: read(2) → length → read(length) → парсинг как UDP-пакет.
 *
 * Backpressure:
 *   send() — BLOCKING с SO_SNDTIMEO=2сек. Если приёмник медленный, send
 *   блокируется → ADPCM-очередь заполняется → I2S дропает кадры
 *   (естественный backpressure, как в UDP). Это avoids deadlock, который
 *   возникал с non-blocking send + select() (ESP ждёт writable, сервер
 *   ждёт данные — никто не двигается).
 *
 * Lifecycle:
 *   tcp_stream_init_listen(port)   — открыть listener, ждать connect в фоне
 *   tcp_stream_is_ready()          — есть активный клиентский коннект
 *   tcp_stream_send(data, len)     — отправить кадр (с framing, blocking)
 *   tcp_stream_close_client()      — закрыть ТОЛЬКО клиент (listener живёт)
 *   tcp_stream_deinit()            — закрыть всё (listener + client + task)
 */

/* Открыть listening socket на port и начать принимать коннекты в фоновой
 * задаче. При новом connect старый коннект закрывается (1 клиент за раз).
 * port = порт из CONFIGURE (семантически переиспользуем, как UDP). */
esp_err_t tcp_stream_init_listen(uint16_t port);

/* Закрыть listener + активный коннект + остановить accept-задачу. */
esp_err_t tcp_stream_deinit(void);

/* FIX (WiFi reconnect): пересоздать listening socket после WiFi
 * disconnect/reconnect. Старый socket становится "zombie" (привязан к
 * уничтоженному netif) и не принимает новые подключения. Без этого
 * сервер не может подключиться после WiFi drop до перезагрузки устройства.
 * No-op если TCP не инициализирован (s_listen_sock < 0). */
esp_err_t tcp_stream_reinit_listener(void);

/* Закрыть ТОЛЬКО активный клиентский коннект (не listener, не accept task).
 * Используется при stream stop — listening socket остаётся живым для
 * быстрого restart без EADDRINUSE. */
void tcp_stream_close_client(void);

/* true, если есть активный клиентский коннект, готовый к send. */
bool tcp_stream_is_ready(void);

/* Отправить аудио-кадр. data = [pkt_header 16B][payload], len = 16+payload.
 * Добавляет 2-байт length-prefix (BE) и пишет в сокет (blocking, SO_SNDTIMEO=2с).
 * При таймауте/обрыве — закрывает клиентский сокет (accept task подберёт новый). */
esp_err_t tcp_stream_send(const uint8_t *data, size_t len);

#endif /* TCP_STREAM_H */
