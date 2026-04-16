import struct
import sqlite3
import argparse
import os
from datetime import datetime, timezone, timedelta

TS_TZ = timezone(timedelta(hours=5))  # local timezone
TS_NOW = datetime.now(TS_TZ)
TS_SKIP = datetime(2026, 3, 22, 0, 0, 0, 0, TS_TZ)  # Skip records before this timestamp

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
		f.seek(0, 2)
		f_size = f.tell()
		ap_cache = {}
		ap_top_offset = f_size
		f.seek(0)

		header = S_HEAD.unpack(f.read(S_HEAD.size))[0]
		if header != MAGIC:
			print(f"Неверный заголовок файла ({header:04X})!")
			return

		while True:
			cur_pos = f.tell()  # for AP debug

			if cur_pos + S_GPS.size >= ap_top_offset:
				print("[ERROR] Current position overlaps AP_Info block!")
				print(f"[DEBUG] Current: 0x{cur_pos:08X} ({cur_pos}), AP top position: 0x{ap_top_offset:08X} ({ap_top_offset})")
				break

			chunk = f.read(S_GPS.size)
			if len(chunk) < S_GPS.size:
				print("[DEBUG] End of file")
				break  # End of file

			ts, lat, lon, alt, acc, bat, cnt = S_GPS.unpack(chunk)
			if ts == 0xFFFFFFFF:
				print("[Info] Timestamp is empty. Stopping...")
				break

			if f.tell() + (S_SIG.size * cnt) >= ap_top_offset:
				print("[Info] GPS+SIG data overlaps AP data!")
				print(f"[DEBUG] Current: 0x{cur_pos:08X} ({cur_pos}), AP top position: 0x{ap_top_offset:08X} ({ap_top_offset})")
				break

			dt = datetime.fromtimestamp(ts + NEOGPS_EPOCH, TS_TZ)
			if dt > TS_NOW:
				print("[ERROR] GPS data position timestamp in future!")
				print(f"[DEBUG] Start: {cur_pos}, Current: {f.tell()}, AP Top: {ap_top_offset}")
				break

			if dt < TS_SKIP:
				f.seek(f.tell() + (S_SIG.size * cnt))
				continue

			print(f"0x{cur_pos:08X}: [{dt.strftime('%d.%m.%Y %H:%M:%S')}] {lat:10.6f} {lon:11.6f} {alt:3d} {acc:3d} {bat:3d}")
			cursor.execute(
				"INSERT INTO gps_log (timestamp, lat, lon, alt, acc, battery) VALUES (?, ?, ?, ?, ?, ?)",
				(ts + NEOGPS_EPOCH, lat, lon, alt, acc, bat))
			gps_id = cursor.lastrowid

			for _ in range(cnt):
				sig_chunk = f.read(S_SIG.size)
				if not sig_chunk:
					break
				try:
					off, rsi = S_SIG.unpack(sig_chunk)
				except Exception as e:
					print(e)
					print(f"0x{f.tell():08X} ({f.tell():8d})")

				if off == 0 or off < f.tell():
					print(
						f"[WARNING] Wrong AP offset (0x{off:04X}) for RSSI [{sig_chunk.hex(' ')}] at 0x{(f.tell() - S_SIG.size):08X}")
					continue

				# Ищем ID по смещению начала структуры AP_Info
				if off not in ap_cache.keys():
					prev_pos = f.tell()
					# Read Info
					f.seek(off)
					mac_bytes, ch_enc, s_len = S_AP.unpack(f.read(S_AP.size))
					# Read SSID
					ssid_pos = off - s_len
					if ssid_pos < 0 or ssid_pos >= f_size:
						print(
							f"[WARNING] Wrong SSID position \"0x{ssid_pos:08X}\" in AP_Info at 0x{off:08x}"
							f" (From RSSI at 0x{(prev_pos - S_SIG.size):08X}. Base at 0x{cur_pos:08X})")
						exit(1)
					if s_len > 32:
						print(
							f"[WARNING] Wrong SSID lenght \"{s_len}\" in AP_Info at 0x{off:08x}"
							f" (From RSSI at 0x{(prev_pos - S_SIG.size):08X}. Base at 0x{cur_pos:08X})")
						exit(2)
					if ch_enc == 0xFF:
						print(
							f"[WARNING] Wrong AP channel/encryption info: 0x{ch_enc:02X} in AP_Info at 0x{off:08x}"
							f" (From RSSI at 0x{(prev_pos - S_SIG.size):08X}. Base at 0x{cur_pos:08X})")
						pass  # Ignored for now

					f.seek(ssid_pos)
					ssid_bytes = f.read(s_len)
					# Prepare data
					ssid = ssid_bytes.decode('utf-8', errors='ignore').strip('\0') or "<HIDDEN>"
					mac_str = ":".join(f"{b:02X}" for b in mac_bytes)
					ch, enc = (ch_enc >> 4) & 0x0F, ch_enc & 0x0F

					# Check: If AP changed her info (MAC-address based unique)
					cursor.execute(
						"SELECT id, ssid, channel, enc_id FROM access_points WHERE mac = ? ORDER BY id DESC LIMIT 1",
						(mac_str,))
					row = cursor.fetchone()

					if row and (row[1] == ssid and row[2] == ch and row[3] == enc):
						ap_db_id = row[0]
					else:
						if row:
							print(f"[INFO] {mac_str} changed:")
							print(f"\t{row[1]} -> {ssid}")
							print(f"\t{row[2]} -> {ch}")
							print(f"\t{row[3]} -> {enc}")
						cursor.execute(
							"INSERT INTO access_points (mac, ssid, channel, enc_id, first_seen) VALUES (?, ?, ?, ?, ?)",
							(mac_str, ssid, ch, enc, int(datetime.now().timestamp())))
						ap_db_id = cursor.lastrowid

					ap_cache[off] = ap_db_id
					f.seek(prev_pos)  # Return back to GPS position
					if ssid_pos < ap_top_offset:
						ap_top_offset = ssid_pos

				cursor.execute(
					"INSERT INTO signals (gps_id, ap_id, rssi) VALUES (?, ?, ?)",
					(gps_id, ap_cache[off], rsi))

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
