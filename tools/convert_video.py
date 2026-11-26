import json
import shutil
import struct
import subprocess
from contextlib import ExitStack
from pathlib import Path
from tempfile import TemporaryDirectory


VIDEO_DIRECTORY = ".video"
PACKET_SIZE = 2048
VIDEO_VERSION = 7
VIDEO_MAX_WIDTH = 240
VIDEO_MAX_HEIGHT = 144


def fingerprint(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f"{value:016x}"


def psmf_files(root):
    for path in sorted(root.rglob("*")):
        if path.relative_to(root).parts[0].startswith(".") or not path.is_file():
            continue
        with path.open("rb") as source:
            if source.read(4) != b"PSMF":
                continue

            header = b"PSMF" + source.read(PACKET_SIZE - 4)
            if len(header) != PACKET_SIZE:
                continue

            offset, size = struct.unpack_from(">II", header, 8)
            if (
                offset < PACKET_SIZE
                or offset % PACKET_SIZE
                or not size
                or size % PACKET_SIZE
                or offset + size > path.stat().st_size
            ):
                continue

            source.seek(offset)
            yield path, header, offset, size, fingerprint(source.read(PACKET_SIZE))


def video_channels(header):
    count = int.from_bytes(header[0x80:0x82], "big")
    for pos in range(0x82, min(len(header) - 15, 0x82 + count * 16), 16):
        if 0xe0 <= header[pos] <= 0xef:
            yield header[pos] & 15


def write_video_timestamps(path, output, track, size, offset, ffprobe):
    command = [
        ffprobe,
        "-v", "error",
        "-select_streams", str(track["index"]),
        "-show_entries", "frame=best_effort_timestamp,pkt_pos",
        "-of", "compact=p=0",
        path,
    ]

    with output.open("wb") as timestamps:
        timestamps.write(struct.pack("<IIII", track["width"], track["height"], *size))

        with subprocess.Popen(command, stdout=subprocess.PIPE, text=True) as probe:
            packet = 1
            previous_pts = 0

            for line in probe.stdout:
                fields = dict(
                    field.split("=", 1)
                    for field in line.strip().split("|")
                    if "=" in field
                )
                if "best_effort_timestamp" not in fields:
                    continue

                pts = fields["best_effort_timestamp"]
                pts = int(pts) if pts != "N/A" else previous_pts + 3003
                position = fields.get("pkt_pos", "N/A")
                if position != "N/A":
                    packet = max(packet, (int(position) - offset) // PACKET_SIZE + 1)

                value = max(0, pts)
                timestamps.write(struct.pack("<III", value & 0xffffffff, value >> 32, packet))
                previous_pts = pts

            if probe.wait():
                raise ValueError(f"deu ruim lendo os quadros de {path.name}")


def convert_video(path, directory, header, offset, ffmpeg, ffprobe):
    command = [
        ffprobe,
        "-v", "error",
        "-select_streams", "v",
        "-show_entries", "stream=index,id,width,height",
        "-of", "json",
        path,
    ]
    tracks = json.loads(subprocess.check_output(command))["streams"]
    channels = list(video_channels(header))

    for index, track in enumerate(tracks):
        channel = int(track["id"], 0) & 15 if "id" in track else channels[index]
        width, height = track["width"], track["height"]
        if not (0 < width <= 480 and 0 < height <= 272):
            raise ValueError(f"tamanho de vídeo zuado em {path.name}")

        if width <= VIDEO_MAX_WIDTH and height <= VIDEO_MAX_HEIGHT:
            size = width, height
        elif width * VIDEO_MAX_HEIGHT > height * VIDEO_MAX_WIDTH:
            size = VIDEO_MAX_WIDTH, max(2, (height * VIDEO_MAX_WIDTH // width) & ~1)
        else:
            size = max(2, (width * VIDEO_MAX_HEIGHT // height) & ~1), VIDEO_MAX_HEIGHT

        output = directory / f"v{channel:02x}"
        write_video_timestamps(path, output.with_suffix(".pts"), track, size, offset, ffprobe)
        video = output.with_suffix(".m2v")
        filters = (
            f"scale={size[0]}:{size[1]}:flags=lanczos,"
            "format=yuv420p,pad=ceil(iw/16)*16:ceil(ih/16)*16,"
            "tpad=stop_mode=clone:stop=1,settb=1/30,setpts=N"
        )

        command = [
            ffmpeg,
            "-nostdin",
            "-v", "error",
            "-xerror",
            "-threads", "2",
            "-i", path,
            "-map", f"0:{track['index']}",
            "-an",
            "-vf", filters,
            "-r", "30",
            "-fps_mode", "cfr",
            "-threads", "2",
            "-c:v", "mpeg2video",
            "-bf", "0",
            "-g", "12",
            "-q:v", "4",
            "-maxrate", "8000k",
            "-bufsize", "1835008",
            "-f", "mpeg2video",
            video,
        ]
        subprocess.run(command, check=True)

        with video.open("ab") as stream:
            stream.write(b"\0\0\1\xb7")


def pes_timestamp(data):
    return (
        ((data[0] & 14) << 29)
        | (data[1] << 22)
        | ((data[2] & 254) << 14)
        | (data[3] << 7)
        | (data[4] >> 1)
    )


def audio_packets(source, size, channels):
    for _ in range(size // PACKET_SIZE):
        packet = source.read(PACKET_SIZE)
        if not packet.startswith(b"\0\0\1\xba") or len(packet) < 14:
            continue

        pos = 14 + (packet[13] & 7)
        while pos + 9 <= len(packet) and packet[pos:pos + 3] == b"\0\0\1":
            end = pos + 6 + int.from_bytes(packet[pos + 4:pos + 6], "big")
            if end > len(packet):
                break

            start = pos + 9 + packet[pos + 8]
            if packet[pos + 3] == 0xbd and start + 4 < end and packet[start] in channels:
                pts = None
                if packet[pos + 7] & 0x80 and packet[pos + 8] >= 5:
                    pts = pes_timestamp(packet[pos + 9:pos + 14])
                yield packet[start], pts, packet[start + 4:end]

            pos = end


def write_audio_frames(track, pending, frame_size, channel_count, filename):
    while len(pending) >= 8:
        start = pending.find(b"\x0f\xd0")
        if start < 0:
            del pending[:-1]
            break

        del pending[:start]
        if len(pending) < 8:
            break

        size = (((pending[2] & 3) << 8) | (pending[3] * 8)) + 8
        if len(pending) < size + 8:
            break

        if frame_size is None:
            flags = (1 << 24) | (1 << 13) | (channel_count << 10) | (size // 8 - 1)
            track.write(b"EA3\0\0\x60\xff\xff" + bytes(24) + struct.pack(">I", flags) + bytes(60))
            frame_size = size
        elif frame_size != size:
            raise ValueError(f"tamanho de áudio mudou em {filename}")

        track.write(pending[8:size + 8])
        del pending[:size + 8]

    return frame_size


def convert_audio(path, directory, header, offset, size, ffmpeg):
    count = int.from_bytes(header[0x80:0x82], "big")
    channels = {
        header[pos + 1]: header[pos + 14]
        for pos in range(0x82, min(len(header) - 15, 0x82 + count * 16), 16)
        if header[pos] == 0xbd and header[pos + 14] in (1, 2)
    }
    if not channels:
        return

    with TemporaryDirectory(dir=directory) as workspace:
        paths = {channel: Path(workspace) / f"{channel:02x}.oma" for channel in channels}
        buffers = {channel: bytearray() for channel in channels}
        first_pts = {}
        frame_sizes = {}

        with ExitStack() as stack, path.open("rb") as source:
            source.seek(offset)
            tracks = {
                channel: stack.enter_context(track_path.open("wb"))
                for channel, track_path in paths.items()
            }

            for channel, pts, payload in audio_packets(source, size, channels):
                if pts is not None:
                    first_pts.setdefault(channel, pts)

                pending = buffers[channel]
                pending.extend(payload)
                frame_size = write_audio_frames(
                    tracks[channel], pending, frame_sizes.get(channel), channels[channel], path.name,
                )
                if frame_size is not None:
                    frame_sizes[channel] = frame_size

        for channel in frame_sizes:
            command = [
                ffmpeg,
                "-nostdin",
                "-v", "error",
                "-xerror",
                "-i", paths[channel],
                "-ac", "2",
                "-ar", "44100",
                "-f", "s16le",
                "pipe:1",
            ]
            with (directory / f"a{channel:02x}.pcm").open("wb") as output:
                output.write(struct.pack("<Q", first_pts.get(channel, 90000)))
                output.flush()
                subprocess.run(command, stdout=output, check=True)


def prepare_videos(root, source):
    destination = root / VIDEO_DIRECTORY
    version = {"version": VIDEO_VERSION, "iso": source}
    stamp = destination / ".source.json"
    if stamp.is_file() and json.loads(stamp.read_text()) == version:
        return

    with TemporaryDirectory(prefix=".video-", dir=root) as staging:
        staging = Path(staging)
        converted = staging / VIDEO_DIRECTORY
        converted.mkdir()
        aliases = {}

        for index, (path, header, offset, size, packet) in enumerate(psmf_files(root)):
            if not index:
                ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
                if not ffmpeg or not ffprobe:
                    raise ValueError("cadê o ffmpeg e o ffprobe no PATH pra converter os vídeos?")

            key = f"{fingerprint(header)}-{packet}"
            directory = converted / key
            if not directory.exists():
                directory.mkdir()
                convert_video(path, directory, header, offset, ffmpeg, ffprobe)
                convert_audio(path, directory, header, offset, size, ffmpeg)

            if packet not in aliases:
                aliases[packet] = key
            elif aliases[packet] != key:
                aliases[packet] = None

        for packet, key in aliases.items():
            if key is not None:
                (converted / f"p{packet}").write_text(key, encoding="ascii")

        (converted / stamp.name).write_text(json.dumps(version) + "\n")

        previous = staging / "previous"
        if destination.exists():
            destination.rename(previous)
        try:
            converted.rename(destination)
        except OSError:
            if previous.exists():
                previous.rename(destination)
            raise
