SET allow_experimental_live_view = 1;

DROP TABLE IF EXISTS test.lv;
DROP TABLE IF EXISTS test.mt;

CREATE TABLE test.mt (a Int32) Engine=MergeTree order by tuple();
CREATE LIVE VIEW test.lv AS SELECT * FROM test.mt;

INSERT INTO test.mt VALUES (1),(2),(3);

SELECT * FROM test.lv FORMAT JSONEachRowWithProgress;

DROP TABLE test.lv;
DROP TABLE test.mt;
