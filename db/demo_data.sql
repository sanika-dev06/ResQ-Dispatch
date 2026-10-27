-- Demo fleet + sample emergencies. Runs on first start and again when an admin
-- presses "Reset Records" in the UI. Users, sessions, the road graph and
-- notifications are not touched.

DELETE FROM dispatches;
DELETE FROM emergencies;
DELETE FROM vehicles;
DELETE FROM sqlite_sequence WHERE name IN ('emergencies', 'dispatches');

-- ALS = Advanced Life Support unit, BLS = Basic Life Support unit.
INSERT INTO vehicles (id, type, status, location, node_id) VALUES
  ('A1', 'ALS', 'Available', 'MG Road',      1),
  ('A2', 'BLS', 'Available', 'FC Road',      2),
  ('A3', 'ALS', 'Available', 'Shivajinagar', 3),
  ('A4', 'BLS', 'Available', 'Kothrud',      4),
  ('A5', 'BLS', 'Available', 'City Centre',  5),
  ('A6', 'ALS', 'On Duty',   'On duty',      NULL),
  ('A7', 'BLS', 'On Duty',   'On duty',      NULL),
  ('A8', 'BLS', 'On Duty',   'On duty',      NULL);

INSERT INTO emergencies (type, priority, location, node_id, patient, details, status, created_at) VALUES
  ('Cardiac Arrest',  'High',   'MG Road, Pune',      1, '', '', 'Waiting', strftime('%Y-%m-%dT%H:%M:%SZ','now','-2 minutes')),
  ('Road Accident',   'High',   'FC Road, Pune',      2, '', '', 'Waiting', strftime('%Y-%m-%dT%H:%M:%SZ','now','-5 minutes')),
  ('Breathing Issue', 'Medium', 'Shivajinagar, Pune', 3, '', '', 'Waiting', strftime('%Y-%m-%dT%H:%M:%SZ','now','-7 minutes')),
  ('Fever',           'Low',    'Kothrud, Pune',      4, '', '', 'Waiting', strftime('%Y-%m-%dT%H:%M:%SZ','now','-10 minutes'));
