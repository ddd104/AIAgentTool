import { spawnSync } from "node:child_process";
import path from "node:path";

const PYTHON_SQLITE_WRITER = String.raw`
import json
import os
import sqlite3
import sys

nodes_path, edges_path, sqlite_path = sys.argv[1:4]
os.makedirs(os.path.dirname(sqlite_path), exist_ok=True)
if os.path.exists(sqlite_path):
    os.remove(sqlite_path)

conn = sqlite3.connect(sqlite_path)
try:
    conn.execute("""
        CREATE TABLE nodes (
            id TEXT PRIMARY KEY,
            type TEXT NOT NULL,
            name TEXT NOT NULL,
            path TEXT NOT NULL,
            system TEXT NOT NULL,
            language TEXT NOT NULL,
            summary TEXT NOT NULL,
            metadata TEXT NOT NULL
        )
    """)
    conn.execute("""
        CREATE TABLE edges (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            from_id TEXT NOT NULL,
            to_id TEXT NOT NULL,
            type TEXT NOT NULL,
            metadata TEXT NOT NULL
        )
    """)
    conn.execute("CREATE INDEX idx_nodes_type_name ON nodes(type, name)")
    conn.execute("CREATE INDEX idx_nodes_path ON nodes(path)")
    conn.execute("CREATE INDEX idx_edges_from ON edges(from_id)")
    conn.execute("CREATE INDEX idx_edges_to ON edges(to_id)")

    with open(nodes_path, "r", encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            node = json.loads(line)
            conn.execute(
                "INSERT OR REPLACE INTO nodes (id, type, name, path, system, language, summary, metadata) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    node["id"],
                    node["type"],
                    node["name"],
                    node["path"],
                    node["system"],
                    node["language"],
                    node["summary"],
                    json.dumps(node.get("metadata", {}), ensure_ascii=False, sort_keys=True),
                ),
            )

    with open(edges_path, "r", encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            edge = json.loads(line)
            conn.execute(
                "INSERT INTO edges (from_id, to_id, type, metadata) VALUES (?, ?, ?, ?)",
                (
                    edge["from"],
                    edge["to"],
                    edge["type"],
                    json.dumps(edge.get("metadata", {}), ensure_ascii=False, sort_keys=True),
                ),
            )

    conn.commit()
finally:
    conn.close()
`;

export interface SqliteWriteResult {
  sqlitePath: string;
  command: string;
}

export function writeSqliteDatabase(nodesPath: string, edgesPath: string, sqlitePath: string): SqliteWriteResult {
  const attempts: Array<{ command: string; args: string[] }> = [
    { command: "python", args: ["-c", PYTHON_SQLITE_WRITER, nodesPath, edgesPath, sqlitePath] },
    { command: "py", args: ["-3", "-c", PYTHON_SQLITE_WRITER, nodesPath, edgesPath, sqlitePath] }
  ];

  const failures: string[] = [];
  for (const attempt of attempts) {
    const result = spawnSync(attempt.command, attempt.args, {
      cwd: path.dirname(sqlitePath),
      encoding: "utf8"
    });
    if (result.status === 0) {
      return { sqlitePath, command: attempt.command };
    }
    failures.push(`${attempt.command}: ${result.stderr || result.error?.message || `exit ${result.status}`}`);
  }

  throw new Error(`Unable to create sqlite database. ${failures.join(" | ")}`);
}
