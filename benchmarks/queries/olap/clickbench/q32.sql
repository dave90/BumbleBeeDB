SELECT WatchID, ClientIP, COUNT(*) AS c, SUM(IsRefresh) AS sum, AVG(ResolutionWidth) AS avg FROM hits GROUP BY WatchID, ClientIP ORDER BY c DESC, WatchID DESC LIMIT 10
