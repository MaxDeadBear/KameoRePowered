"""Runs ida_query.py headless against the title-update database (tu) or a copy of the named vanilla one (vanilla),
reading out/tu/query.txt and printing out/tu/answer.txt. Usage: ida_run.py tu|vanilla [database folder]
Queries come as further arguments ("decompile 82430418" ...) or from out/tu/query.txt when there are none."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "out", "tu")
IDA = "E:/IDA/idat.exe"


def main() -> int:
    db = sys.argv[1] if len(sys.argv) > 1 else "tu"
    folder = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.environ["TEMP"], "kameo_ida")
    queries = sys.argv[3:]
    if queries:
        open(os.path.join(OUT, "query.txt"), "w").write("\n".join(queries) + "\n")
    database = os.path.join(folder, "tu.bin.i64" if db == "tu" else "kameo.i64")
    subprocess.call([IDA, "-A", f"-S{os.path.join(HERE, 'ida_query.py')}", f"-L{os.path.join(folder, 'query_' + db + '.log')}", database])
    sys.stdout.reconfigure(encoding="utf-8")
    print(open(os.path.join(OUT, "answer.txt"), encoding="utf-8").read())
    return 0


if __name__ == "__main__":
    sys.exit(main())
