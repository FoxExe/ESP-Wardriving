import struct
from datetime import datetime

NEOGPS_EPOCH = 946684800
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


def parse_log(filename):
	with open(filename, "rb") as f:
		header = S_HEAD.unpack(f.read(S_HEAD.size))[0]
		if header != MAGIC:
			print(f"Wrong file header ({header:04X})!")
			return

		ap_info = {}
		f.seek(-S_AP.size, 2)  # go to end of file
		while True:
			pos = f.tell()
			mac, ch_enc, len = S_AP.unpack(f.read(S_AP.size))
			if len == 0xFF:
				break
			ch = (ch_enc >> 4) & 0x0F
			enc = ch_enc & 0x0F
			f.seek(f.tell() - S_AP.size - len)
			ssid = f.read(len).decode('utf-8').strip('\0').strip()
			print(f"0x{pos:08X}:", end=" ")
			for c in mac:
				print(f"{c:02X}", end=" ")
			print(f"- {ENC_TYPES.get(enc, "UNKNOWN"):>8s} @ {ch:2d} \"{ssid}\" ")

			ap_info[pos] = (mac, ssid, ch, enc)

			f.seek(pos - len - S_AP.size)

		print("-" * 32)

		f.seek(2)  # skip magic
		p_num = 0
		while True:
			pos = f.tell()
			ts, lat, lon, alt, acc, bat, cnt = S_GPS.unpack(f.read(S_GPS.size))
			if ts == 0xFFFFFFFF:
				break
			dt = datetime.fromtimestamp(ts + NEOGPS_EPOCH)
			print(f"[{dt}] #{p_num:04d} @ {pos:08X}: {lat:9.5f}, {lon:10.5f}, {alt:5d}m, {acc:3d}m, {bat:3d}%")
			for i in range(cnt):
				off, rsi = S_SIG.unpack(f.read(S_SIG.size))
				print(f"\t#{i + 1:2d} @ {off:5d}: {rsi}dbm", end="")
				info = ap_info.get(off, None)
				if info:
					print(f" ({info[2]:2d} {ENC_TYPES.get(info[3], "UNKNOWN"):>8s} \"{info[1]}\")")
				else:
					print()  # newline

			# align to 4 bytes:
			#curr = f.tell()
			#f.seek(curr - curr % 4)

			p_num += 1


if __name__ == "__main__":
	# NOTE: python.exe -X utf8 log_reader.py > log_dump_8.txt
	parse_log("log_dump_8.bin")
