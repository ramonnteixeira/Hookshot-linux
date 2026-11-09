"""Background threads never read the replay's uniform update list.

ub_updates is the replay thread's: it grows and is pruned (pop_front) while shader translation, pipeline builds and the
startup precompile run on other threads. Those threads read guest memory from the captures only (ub_limit 0), and with
ub_limit 0 not even the list's size is read.
python -I -B tests/test_ub_threads.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker, start=0):
    start = text.index(marker, start)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def first_run(body):
    """Where the body first runs a job: fn(); or run_guarded(fn); (a job that throws is logged, vk_jobs.cpp)"""
    at = [body.index(m) for m in ("run_guarded(fn);", "fn();") if m in body]
    if not at:
        raise SystemExit("no job run found")
    return min(at)


def main():
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    if "void ub_thread_off(" not in replay:
        raise SystemExit("no ub_thread_off: background threads read the update list")
    off = block(replay, "void ub_thread_off(")
    if "ub_limit = 0;" not in off:
        raise SystemExit("ub_thread_off does not keep the thread out of the update list")
    for marker in ("bool ub_read(", "uint64_t ub_overlay("):
        body = block(replay, marker)
        lim = re.search(r"const size_t lim = ([^;]+);", body)
        if not lim or not lim.group(1).startswith("ub_limit ?"):
            raise SystemExit(f"{marker} reads the update list's size on a background thread")

    if "void ub_thread_off();" not in (ROOT / "gpu" / "vk" / "vk_draw.h").read_text(encoding="utf-8"):
        raise SystemExit("ub_thread_off is not declared for the other files")

    jobs = (ROOT / "gpu" / "vk" / "vk_jobs.cpp").read_text(encoding="utf-8")
    worker = block(jobs, "void worker(")
    if "ub_thread_off();" not in worker or worker.index("ub_thread_off();") > first_run(worker):
        raise SystemExit("a shared worker runs jobs before leaving the update list")
    serial = block(jobs, "void jobs_push_serial(")
    if "ub_thread_off();" not in serial or serial.index("ub_thread_off();") > first_run(serial):
        raise SystemExit("the serial build thread runs jobs before leaving the update list")

    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    pools = [m.start() for m in re.finditer(r"threads\.emplace_back\(worker\)", draw)]
    if not pools:
        raise SystemExit("no precompile thread pools found: update this check")
    for at in pools:
        lambda_at = draw.rindex("const auto worker = [", 0, at)
        body = block(draw, "const auto worker = [", lambda_at)
        first = body[body.index("{") + 1:].lstrip()
        if not first.startswith("ub_thread_off();"):
            raise SystemExit("a precompile worker translates before leaving the update list")
    print("PASS: background threads stay out of the replay's uniform update list")


if __name__ == "__main__":
    main()
