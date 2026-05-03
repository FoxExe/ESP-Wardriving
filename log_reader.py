import struct
import sys
import os
from datetime import datetime, timedelta

LOG_DUMP = "log_72.bin"


BLOCK_SIZE = 64 * 1024
MAGIC = 0xDDCC

MAX_GPS_PAUSE = 10 * 60  # Max pause between records. GPS Tracks separator for statistics

# NOTE: H=uint16, I=uint32, B=uint8, f=float, b=int8 (signed)
S_HEAD = struct.Struct('<H')
S_GPS = struct.Struct('<IffhHBB')
S_AP = struct.Struct('<6sBB')
S_SIG = struct.Struct('<Hb')

ENC_TYPES = {
	2: 'WPA',
	4: 'WPA2',
	5: 'WEP',
	7: 'OPEN',
	8: 'WPA/WPA2'
}


def to_hex_str(mac: bytes, separator: str = ":"):
	text = []
	for c in mac:
		text.append(f"{c:02X}")
	return separator.join(text)


if __name__ == "__main__":
	# NOTE: If not work, try this:
	# python.exe -X utf8 log_reader.py > log.txt

	if sys.platform == "win32":
		os.system('chcp 65001 > nul')
	sys.stdout.reconfigure(encoding='utf-8')
	sys.stderr.reconfigure(encoding='utf-8')

	if not os.path.exists(LOG_DUMP):
		print(f"File {LOG_DUMP} not found!")
		exit(1)

	block_stats = {}
	last_ts = 0
	last_pts = 0
	tracks = []

	gps_cnt = 0
	track_start_ts = None
	track_start_pos = None

	with open(LOG_DUMP, "rb") as f:
		f.seek(0, os.SEEK_END)
		file_size = f.tell()
		f.seek(0)

		if file_size < BLOCK_SIZE or file_size % BLOCK_SIZE != 0:
			print("Wrong file size!")
			exit(1)

		for blk_id in range(file_size // BLOCK_SIZE):
			blk_start = blk_id * BLOCK_SIZE
			f.seek(blk_start)

			gps_cnt = 0
			aps = {}

			header = S_HEAD.unpack(f.read(S_HEAD.size))[0]
			if header != MAGIC:
				print(f"Wrong block header ({header:04X}) at {blk_id * BLOCK_SIZE} (Block #{blk_id})!")
				exit(2)

			while (pos := f.tell()) < blk_start + BLOCK_SIZE:
				ts, lat, lon, alt, acc, bat, cnt = S_GPS.unpack(f.read(S_GPS.size))

				if ts == 0xFFFFFFFF:
					break

				if track_start_pos is None:
					track_start_ts = ts
					track_start_pos = pos
					last_pts = gps_cnt
				elif ts - last_ts > MAX_GPS_PAUSE:
					tracks.append((track_start_pos, blk_id, gps_cnt - last_pts, track_start_ts, last_ts))
					track_start_pos = pos
					track_start_ts = ts
					last_pts = gps_cnt

				last_ts = ts
				gps_cnt += 1
				dt = datetime.fromtimestamp(ts)

				print(f"--- Point #{gps_cnt} in block #{blk_id} --")
				print(f"0x{pos:08X} >>>> [{dt}]  {lat:.5f},{lon:.5f}  ALT: {alt}m  ACC: {acc}m  BAT: {bat}%")
				for i in range(cnt):
					pos_rssi = f.tell()
					data = f.read(S_SIG.size)
					off, rsi = S_SIG.unpack(data)

					print(f"0x{pos_rssi:08X} - {i + 1:2d}", end=" ")

					if rsi > 0 or rsi < -100:
						print(f"[ERROR] Wrong RSSI: \"{rsi}\"!")
						continue

					if off <= 0 or off > BLOCK_SIZE:
						print(f"[ERROR] Wrong AP offset: \"{off}\" (Total AP count: {len(aps)})!")
						continue

					print(f"{rsi}dbm", end="  ")

					# Read AP info
					prev_pos = f.tell()
					f.seek(blk_start + off)
					mac, ch_enc, ssid_len = S_AP.unpack(f.read(S_AP.size))
					if ssid_len > 32 or ssid_len < 0:
						ssid = f"[ERROR] Wrong SSID size: {ssid_len}"
					else:
						f.seek(blk_start + off - ssid_len)
						data = f.read(ssid_len)
						try:
							ssid = data.decode('utf-8').strip('\0').strip()
						except Exception as e:
							ssid = "[ERROR] Not a UTF-8 string: " + str(data)

						ssid = "[ HIDDEN ]" if ssid == "" else ssid
					ch = (ch_enc >> 4) & 0x0F
					enc = ch_enc & 0x0F
					aps[mac] = ssid  # For statistic

					# Go back to GPS RSSI records
					f.seek(prev_pos)
					print(f"[0x{(blk_start + off):08X}] ch. {ch:<2d}  {to_hex_str(mac)}  {ENC_TYPES.get(enc, "UNKNOWN"):<8s}  {ssid}")

			block_stats[blk_id] = (gps_cnt, len(aps))

	if track_start_pos is not None:
		tracks.append((track_start_pos, blk_id, gps_cnt - last_pts, track_start_ts, last_ts))

	print("-" * 64)
	print(f"{'Offset':<8} | Block | Track | {'Start Time':<19} | {'End Time':<19} | Duration | Points")
	for i, s in enumerate(tracks, 1):
		pos, blk, cnt, ts_start, ts_end = s
		dts = datetime.fromtimestamp(ts_start)
		dte = datetime.fromtimestamp(ts_end)
		dtr = dte - dts
		print(f"{pos:08X} | {blk:>5} | {i:>5} | {dts} | {dte} | {str(dtr):>8} | {cnt:>6}")

	print("-" * 64)
	print("Block   GPS  APs")
	for i, s in block_stats.items():
		print(f"{i:5d}  {s[0]:4d}  {s[1]:4d}")
