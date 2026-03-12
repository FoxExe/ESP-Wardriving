import os
import gzip
import shutil
from os.path import join, isdir, relpath
from platformio import util

Import("env")

SRC_WEB_DIR = join(env.subst("$PROJECT_DIR"), "src_web")
DATA_DIR = join(env.subst("$PROJECT_DIR"), "data")


def gzip_web_files(source, target, env):
	print("[Gzip Web Assets] Compressing web...")

	# 1. Очистка старой папки data (чтобы не было "мусора")
	if os.path.exists(DATA_DIR):
		shutil.rmtree(DATA_DIR)
	os.makedirs(DATA_DIR)

	# 2. Обход src_web и упаковка
	for root, dirs, files in os.walk(SRC_WEB_DIR):
		for file in files:
			# Игнорируем системные файлы (например, .DS_Store)
			if file.startswith('.'):
				continue

			src_path = join(root, file)
			rel_p = relpath(root, SRC_WEB_DIR)
			target_dir = join(DATA_DIR, rel_p)

			if not os.path.exists(target_dir):
				os.makedirs(target_dir)

			target_path = join(target_dir, file + ".gz")

			print(f"Compressing: {file} -> {file}.gz")
			with open(src_path, 'rb') as f_in:
				with gzip.open(target_path, 'wb', compresslevel=9) as f_out:
					shutil.copyfileobj(f_in, f_out)

	print("[Gzip Web Assets] Done!")


if not os.path.exists(DATA_DIR):
	print(f"[Gzip Web Assets]: \"{DATA_DIR}\" is missed! Created.")
	os.makedirs(DATA_DIR)

env.AddPreAction("buildfs", gzip_web_files)
#env.AlwaysBuild("buildfs")
