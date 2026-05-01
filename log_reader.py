import struct
import sys
import os
from datetime import datetime


LOG_DUMP = "log_67-71.bin"


BLOCK_SIZE = 64 * 1024
MAGIC = 0xDDCC

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


def hex_string(mac: bytes, separator: str = ":"):
	lines = []
	for c in mac:
		lines.append(f"{c:02X}")
	return separator.join(lines)


if __name__ == "__main__":
	# NOTE: If not work, try this:
	# python.exe -X utf8 log_reader.py > log_dump_8.txt

	if sys.platform == "win32":
		os.system('chcp 65001 > nul')
	sys.stdout.reconfigure(encoding='utf-8')
	sys.stderr.reconfigure(encoding='utf-8')

	with open(LOG_DUMP, "rb") as f:
		f.seek(0, os.SEEK_END)
		file_size = f.tell()
		f.seek(0)

		if file_size < BLOCK_SIZE or file_size % BLOCK_SIZE != 0:
			print("Wrong file size!")
			exit(1)

		stats = {}

		for blk_id in range(file_size // BLOCK_SIZE):
			blk_start = blk_id * BLOCK_SIZE
			f.seek(blk_start)

			gps_cnt = 0
			aps = {}

			header = S_HEAD.unpack(f.read(S_HEAD.size))[0]
			if header != MAGIC:
				print(f"Wrong block header ({header:04X}) at {blk_id * BLOCK_SIZE} (Block #{blk_id})!")
				exit(2)

			while (pos := f.tell()) < file_size:
				ts, lat, lon, alt, acc, bat, cnt = S_GPS.unpack(f.read(S_GPS.size))

				if ts == 0xFFFFFFFF:
					break

				dt = datetime.fromtimestamp(ts)

				gps_cnt += 1
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
					print(f"[0x{(blk_start + off):08X}] ch. {ch:<2d}  {hex_string(mac)}  {ENC_TYPES.get(enc, "UNKNOWN"):<8s}  {ssid}")

			stats[blk_id] = (gps_cnt, len(aps))

		print("-" * 64)
		print("Block  GPS  APs")
		for i, s in stats.items():
			print(f"{(i + 1):5d} {s[0]:4d} {s[1]:4d}")


"""
		ap_info = {}
		f.seek(-S_AP.size, 2)  # go to end of file
		while True:
			pos = f.tell()
			mac, ch_enc, size = S_AP.unpack(f.read(S_AP.size))
			if size == 0xFF:
				break
			ch = (ch_enc >> 4) & 0x0F
			enc = ch_enc & 0x0F
			f.seek(f.tell() - S_AP.size - size)
			ssid = f.read(size).decode('utf-8').strip('\0').strip()
			print(f"0x{pos:08X}:", end=" ")
			for c in mac:
				print(f"{c:02X}", end=" ")
			print(f"- {ENC_TYPES.get(enc, "UNKNOWN"):>8s} @ {ch:2d} \"{ssid}\" ")

			ap_info[pos] = (mac, ssid, ch, enc)

			f.seek(pos - size - S_AP.size)
"""
