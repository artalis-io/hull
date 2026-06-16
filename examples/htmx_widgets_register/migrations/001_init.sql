-- Tiny asset register schema. One table is enough to exercise the
-- whole widget tier: sortable + searchable + paginated + inline-
-- editable + deletable rows.

CREATE TABLE assets (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT    NOT NULL,
    category   TEXT    NOT NULL DEFAULT 'general',
    status     TEXT    NOT NULL DEFAULT 'active',
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- Seed a couple dozen rows so pagination has something to show.
INSERT INTO assets (name, category, status) VALUES
    ('Pump #7',              'hydraulic', 'active'),
    ('Conveyor B-12',        'electric',  'active'),
    ('Drill A',              'power',     'active'),
    ('Forklift #3',          'vehicle',   'maintenance'),
    ('Compressor T-5',       'pneumatic', 'active'),
    ('Generator G-1',        'electric',  'active'),
    ('Welder W-4',           'electric',  'retired'),
    ('Lathe L-2',            'machine',   'active'),
    ('Mill M-6',             'machine',   'active'),
    ('Crane C-9',            'vehicle',   'active'),
    ('Truck T-1',            'vehicle',   'maintenance'),
    ('Hoist H-3',            'hydraulic', 'active'),
    ('Bandsaw B-7',          'machine',   'active'),
    ('Grinder G-12',         'electric',  'active'),
    ('Press P-8',            'hydraulic', 'retired'),
    ('Punch P-15',           'pneumatic', 'active'),
    ('Sander S-22',          'electric',  'active'),
    ('Router R-44',          'electric',  'active'),
    ('Polisher P-3',         'electric',  'maintenance'),
    ('Drill Press D-99',     'power',     'active');
