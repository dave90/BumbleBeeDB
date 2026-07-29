SELECT AdvEngineID, COUNT(*) AS count FROM hits WHERE AdvEngineID <> 0 GROUP BY AdvEngineID ORDER BY count DESC
