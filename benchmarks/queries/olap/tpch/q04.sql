SELECT
    o_orderpriority,
    count(*) AS order_count
FROM orders
WHERE o_orderdate >= '1993-07-01'
  AND o_orderdate < '1993-10-01'
  AND 0 < (
    SELECT COUNT(*)
    FROM lineitem
    WHERE l_orderkey = o_orderkey
      AND l_commitdate < l_receiptdate)
GROUP BY o_orderpriority
ORDER BY o_orderpriority
LIMIT 1000
