import struct
import sqlite3
import argparse
import os
from datetime import datetime

NEOGPS_EPOCH = 946684800
MAGIC = 0xDDCC

S_HEAD = struct.Struct('<H')
S_GPS = struct.Struct('<IffhHBB')
S_AP = struct.Struct('<6sBB')
S_SIG = struct.Struct('<Hb')

ENC_TYPES = {2: 'WPA', 4: 'WPA2', 5: 'WEP', 7: 'OPEN', 8: 'WPA/WPA2'}


def init_db(db_path):
	conn = sqlite3.connect(db_path)
	cursor = conn.cursor()

	# Encryption types
	cursor.execute("CREATE TABLE IF NOT EXISTS encryption_types (id INTEGER PRIMARY KEY, name TEXT)")
	cursor.executemany("INSERT OR IGNORE INTO encryption_types VALUES (?, ?)", ENC_TYPES.items())

	# AP Info table
	# NOTE: Better to use BIGINT for store MAC address
	cursor.execute("""
		CREATE TABLE IF NOT EXISTS access_points (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			mac TEXT,
			ssid TEXT,
			channel INTEGER,
			enc_id INTEGER,
			first_seen DATETIME DEFAULT CURRENT_TIMESTAMP
		)
	""")
	cursor.execute("CREATE INDEX IF NOT EXISTS idx_ap_mac ON access_points(mac)")

	# GPS Position info (and extended info)
	cursor.execute("""
		CREATE TABLE IF NOT EXISTS gps_log (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			timestamp DATETIME,
			lat REAL, lon REAL, alt INTEGER, acc INTEGER, battery INTEGER
		)
	""")

	# Wi-Fi signal records
	cursor.execute("""
		CREATE TABLE IF NOT EXISTS signals (
			gps_id INTEGER,
			ap_id INTEGER,
			rssi INTEGER,
			FOREIGN KEY(gps_id) REFERENCES gps_log(id),
			FOREIGN KEY(ap_id) REFERENCES access_points(id)
		)
	""")
	conn.commit()
	return conn

def parse_log(filename, db_path):
	if not os.path.exists(filename):
		print(f"Файл {filename} не найден.")
		return

	conn = init_db(db_path)
	cursor = conn.cursor()

	with open(filename, "rb") as f:
		header = S_HEAD.unpack(f.read(S_HEAD.size))[0]
		if header != MAGIC:
			print(f"Неверный заголовок файла ({header:04X})!")
			return

		f.seek(-S_AP.size, 2)
		pos = f.tell()
		offset_to_db_id = {}

		while pos > 0:
			f.seek(pos)
			mac_bytes, ch_enc, s_len = S_AP.unpack(f.read(S_AP.size))
			if s_len == 0xFF:
				break

			ssid_pos = pos - s_len
			if ssid_pos < 0:
				break

			f.seek(ssid_pos)
			ssid_bytes = f.read(s_len)
			ssid = ssid_bytes.decode('utf-8', errors='ignore').strip('\0') or "<HIDDEN>"

			mac_str = ":".join(f"{b:02X}" for b in mac_bytes)
			ch, enc = (ch_enc >> 4) & 0x0F, ch_enc & 0x0F

			cursor.execute(
				"SELECT id, ssid, channel, enc_id FROM access_points WHERE mac = ? ORDER BY id DESC LIMIT 1",
				(mac_str,))
			row = cursor.fetchone()

			if row and (row[1] == ssid and row[2] == ch and row[3] == enc):
				ap_db_id = row[0]
			else:
				if row:
					print(f"[!] New version for {mac_str}: {row[1]} -> {ssid}")
				cursor.execute(
					"INSERT INTO access_points (mac, ssid, channel, enc_id, first_seen) VALUES (?, ?, ?, ?, ?)",
					(mac_str, ssid, ch, enc, int(datetime.now().timestamp())))
				ap_db_id = cursor.lastrowid

			offset_to_db_id[pos] = ap_db_id

			pos = ssid_pos - S_AP.size

		f.seek(2)
		while True:
			chunk = f.read(S_GPS.size)
			if len(chunk) < S_GPS.size:
				break

			ts, lat, lon, alt, acc, bat, cnt = S_GPS.unpack(chunk)
			if ts == 0xFFFFFFFF:
				break

			cursor.execute(
				"INSERT INTO gps_log (timestamp, lat, lon, alt, acc, battery) VALUES (?, ?, ?, ?, ?, ?)",
				(ts + NEOGPS_EPOCH, lat, lon, alt, acc, bat))
			gps_id = cursor.lastrowid

			for _ in range(cnt):
				sig_chunk = f.read(S_SIG.size)
				if not sig_chunk:
					break
				off, rsi = S_SIG.unpack(sig_chunk)

				# Ищем ID по смещению начала структуры AP_Info
				if off in offset_to_db_id:
					cursor.execute(
						"INSERT INTO signals (gps_id, ap_id, rssi) VALUES (?, ?, ?)",
						(gps_id, offset_to_db_id[off], rsi))

	conn.commit()
	conn.close()


if __name__ == "__main__":
	parser = argparse.ArgumentParser()
	parser.add_argument("files", nargs='+')
	parser.add_argument("-o", "--output", default="database.sqlite")
	args = parser.parse_args()

	for f in args.files:
		print(f"Processing {f}...")
		parse_log(f, args.output)
