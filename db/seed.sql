-- Road graph for Dijkstra. Runs once, on the first start of an empty database.
--
-- NOTE: the distances below are rough, illustrative values chosen for this
-- project - they are NOT surveyed road distances. Edit the `edges` table to
-- change them; the server reads the graph when it starts.

INSERT INTO nodes (id, name, aliases) VALUES
  (1,  'MG Road',       'm g road,mahatma gandhi road,camp'),
  (2,  'FC Road',       'f c road,fergusson college road'),
  (3,  'Shivajinagar',  'shivaji nagar'),
  (4,  'Kothrud',       ''),
  (5,  'City Centre',   'city center'),
  (6,  'Deccan',        'deccan gymkhana'),
  (7,  'Swargate',      ''),
  (8,  'Pune Station',  'railway station,pune railway station'),
  (9,  'Koregaon Park', ''),
  (10, 'Hadapsar',      ''),
  (11, 'Baner',         '');

INSERT INTO edges (a, b, km) VALUES
  (1, 8,  1.8),   -- MG Road       - Pune Station
  (1, 7,  2.5),   -- MG Road       - Swargate
  (1, 5,  2.2),   -- MG Road       - City Centre
  (1, 9,  3.5),   -- MG Road       - Koregaon Park
  (1, 10, 7.5),   -- MG Road       - Hadapsar
  (5, 7,  2.0),   -- City Centre   - Swargate
  (5, 6,  2.4),   -- City Centre   - Deccan
  (5, 8,  2.5),   -- City Centre   - Pune Station
  (6, 2,  1.0),   -- Deccan        - FC Road
  (6, 4,  3.5),   -- Deccan        - Kothrud
  (6, 3,  2.5),   -- Deccan        - Shivajinagar
  (2, 3,  1.5),   -- FC Road       - Shivajinagar
  (2, 4,  4.5),   -- FC Road       - Kothrud
  (3, 8,  4.0),   -- Shivajinagar  - Pune Station
  (3, 11, 5.5),   -- Shivajinagar  - Baner
  (4, 11, 6.5),   -- Kothrud       - Baner
  (9, 8,  3.0),   -- Koregaon Park - Pune Station
  (9, 10, 6.5),   -- Koregaon Park - Hadapsar
  (10, 7, 8.0);   -- Hadapsar      - Swargate

-- Base value shown on the "Dispatched Today" card (the original UI started at 28).
-- Set to 0 for a purely real count.
INSERT INTO meta (key, value) VALUES ('dispatched_baseline', '28');
