-- A CPU-bound SQLite workload for the benchmark: in-memory tables,
-- bulk insert, index build, joins, aggregates, sorting and string work.
PRAGMA journal_mode = OFF;
PRAGMA synchronous = OFF;
CREATE TABLE a(id INTEGER PRIMARY KEY, k INT, s TEXT, x REAL);
WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 300000)
INSERT INTO a SELECT i, (i * 2654435761) % 100003, printf('row%08d', (i * 7919) % 300000), i * 0.37 FROM n;
CREATE INDEX ak ON a(k);
CREATE TABLE b AS SELECT k, count(*) AS c, sum(x) AS sx FROM a GROUP BY k;
SELECT count(*), sum(c), round(sum(sx), 2) FROM b;
SELECT count(*) FROM a JOIN b USING (k) WHERE b.c > 2;
SELECT s FROM a ORDER BY s DESC LIMIT 3;
SELECT k, max(length(s || k)), min(x) FROM a WHERE k % 97 = 5 GROUP BY k ORDER BY k LIMIT 5;
SELECT count(DISTINCT substr(s, 4, 3)) FROM a;
