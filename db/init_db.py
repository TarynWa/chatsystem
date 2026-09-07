#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
分布式 IM 系统 - 数据库初始化脚本 (Python 版)

作用: 执行 db/init.sql, 创建 im_auth / im_message / im_social 三库及全部基础表。
初始化脚本(init.sql)是唯一 schema 来源, 本脚本只负责把它灌进 MySQL。

后端选择(按优先级):
  1. PyMySQL           (pip install pymysql)
  2. mysql.connector   (pip install mysql-connector-python)
  3. mysql 客户端二进制 (系统装有 mysql-client 即可, 无需任何 Python 依赖)

用法:
  python3 db/init_db.py                                  # 默认 localhost root(本地 socket 免密)
  python3 db/init_db.py -h 127.0.0.1 -P 3306 -u root -p # TCP 连接, 交互式输密码
  python3 db/init_db.py -p 'secret' --schema /path/init.sql
"""

import argparse
import getpass
import os
import subprocess
import sys

# schema 文件默认与本脚本同目录的 init.sql
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SCHEMA = os.path.join(HERE, "init.sql")

# 需要出现在信息里的库前缀(校验用)
DB_PREFIX = "im_"


# ---------------------------------------------------------------------------
# 连接参数
# ---------------------------------------------------------------------------
def parse_args():
    p = argparse.ArgumentParser(
        description="初始化分布式 IM 系统数据库(执行 db/init.sql)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("-H", "--host", default="localhost", help="MySQL 主机(localhost 走本地 socket)")
    p.add_argument("-P", "--port", type=int, default=3306, help="MySQL 端口")
    p.add_argument("-u", "--user", default="root", help="MySQL 用户")
    p.add_argument(
        "-p", "--password", nargs="?", const="__PROMPT__", default=None,
        help="密码。带 -p 不给值则交互输入; 省略则不发密码(依赖本地 socket auth)",
    )
    p.add_argument("--schema", default=DEFAULT_SCHEMA, help="要执行的建库脚本路径")
    return p.parse_args()


def resolve_password(raw):
    """None -> 不发密码; '__PROMPT__' -> 交互输入; 其它 -> 原样返回。"""
    if raw == "__PROMPT__":
        return getpass.getpass("MySQL password: ")
    return raw  # None 或明文


# ---------------------------------------------------------------------------
# 后端 1/2: Python 数据库驱动
# ---------------------------------------------------------------------------
def _try_driver_execute(host, port, user, password, schema_text):
    """优先 PyMySQL, 其次 mysql.connector。都装时抛 ImportError。返回 (表名列表)。"""
    tables = []
    try:
        import pymysql
        from pymysql.constants import CLIENT

        conn = pymysql.connect(
            host=host, port=port, user=user,
            password=password or "",
            client_flag=CLIENT.MULTI_STATEMENTS,
            autocommit=True,
        )
        with conn.cursor() as cur:
            cur.execute(schema_text)  # 整脚本含多条语句
            cur.execute(
                "SELECT table_schema, table_name FROM information_schema.tables "
                "WHERE table_schema LIKE 'im\\_%'"
            )
            tables = [row for row in cur.fetchall()]
        conn.close()
        return tables, "pymysql"
    except ImportError:
        pass

    # 未安装则 ImportError 上抛, 由 main 回退到 mysql CLI 后端
    import mysql.connector

    conn = mysql.connector.connect(
        host=host, port=port, user=user,
        password=password or "",
        autocommit=True,
    )
    cur = conn.cursor()
    for _ in cur.execute(schema_text, multi=True):
        pass  # multi=True: 逐条执行完所有语句
    cur.execute(
        "SELECT table_schema, table_name FROM information_schema.tables "
        "WHERE table_schema LIKE 'im\\_%'"
    )
    tables = [row for row in cur.fetchall()]
    conn.close()
    return tables, "mysql.connector"


# ---------------------------------------------------------------------------
# 后端 3: mysql 客户端二进制
# ---------------------------------------------------------------------------
def _run_mysql_cli(args, password, sql_bytes=None, extra=None):
    """执行 `mysql ...`。sql_bytes 给到 stdin; extra 追加命令行参数。返回 stdout(bytes)。"""
    env = dict(os.environ)
    if password:  # 用环境变量传密码, 避免出现在 ps 里
        env["MYSQL_PWD"] = password

    cmd = [
        "mysql", "--host=" + args.host, "--port=" + str(args.port),
        "--user=" + args.user,
    ]
    if extra:
        cmd += extra
    try:
        proc = subprocess.run(
            cmd, input=sql_bytes, capture_output=True, env=env,
        )
    except FileNotFoundError:
        sys.exit(
            "未找到 mysql 客户端二进制, 且未安装 Python 驱动。\n"
            "请任选其一: 1) apt install mysql-client  2) pip install pymysql"
        )
    if proc.returncode != 0:
        detail = proc.stderr.decode("utf-8", "replace")
        sys.stderr.write(f"mysql 执行失败 (exit={proc.returncode}):\n{detail}\n")
        sys.exit(proc.returncode)
    return proc.stdout


def _cli_list_tables(args, password):
    out = _run_mysql_cli(
        args, password,
        extra=[
            "-N", "-e",
            ("SELECT table_schema, table_name FROM information_schema.tables "
             "WHERE table_schema LIKE 'im\\_%'"),
        ],
    )
    rows = []
    for line in out.decode("utf-8", "replace").splitlines():
        if "\t" in line:
            rows.append(line.split("\t", 1))
    return rows
# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    args = parse_args()
    password = resolve_password(args.password)

    if not os.path.isfile(args.schema):
        sys.exit(f"schema 文件不存在: {args.schema}")
    with open(args.schema, "r", encoding="utf-8") as f:
        schema_text = f.read()

    # 1) 优先驱动后端
    try:
        tables, backend = _try_driver_execute(
            args.host, args.port, args.user, password, schema_text
        )
    except ImportError:
        # 2) 回退 mysql 客户端
        with open(args.schema, "rb") as f:
            _run_mysql_cli(args, password, sql_bytes=f.read())
        tables = _cli_list_tables(args, password)
        backend = "mysql CLI"

    # 3) 汇总校验结果
    by_db = {}
    for schema, table in tables:
        if schema.startswith(DB_PREFIX):
            by_db.setdefault(schema, []).append(table)

    print(f"\n✔ 初始化完成 (后端: {backend})")
    if not by_db:
        print(f"   未检测到 {DB_PREFIX} 前缀的库, 请检查 schema 内容。")
        return
    for schema, tb in sorted(by_db.items()):
        count = len(tb)
        names = ", ".join(sorted(tb))
        print(f"  {schema:<12} {count:>2} 张表: {names}")


if __name__ == "__main__":
    main()
