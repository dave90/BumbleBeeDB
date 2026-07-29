SELECT SearchPhrase AS s, COUNT(*) AS c FROM hits WHERE SearchPhrase <> '' GROUP BY SearchPhrase ORDER BY c DESC, SearchPhrase DESC LIMIT 6
