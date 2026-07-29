- window
- join ordering: real HLL-based NDV (not implemented). The cost-based join reorderer
  (src/optimizer/join_order/, see plan_join_ord.md) currently derives each join key's distinct count
  from row counts alone (union-find over equi-joined columns; class NDV = smallest table row count).
  This is exact for star/snowflake schemas (TPC-H) but breaks for non-PK/FK joins, filtered NDV, and
  many-to-many joins. Add a HyperLogLog sketch (port ~/git/BumbleBee/third_party/hyperloglog) +
  \analyze + per-column NDV in TableStats, feeding JoinEdge.ndv_ (field + estimator already wired).
  Obstacle: catalog ParquetTable::MakeScan is a stub that throws — parquet NDV must go through the
  parquet reader / ParquetTableOps, not TableStorage::MakeScan.
- external tables schema evolution (per-file schema resolution: missing col -> NULL, extra -> ignore, widen -> cast; maybe ALTER TABLE for external only — see plan_external.md §6)
- python
- Aggregates not clean
- SIMD