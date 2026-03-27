CREATE TABLE IF NOT EXISTS products (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    category TEXT NOT NULL,
    price REAL NOT NULL
);

INSERT OR IGNORE INTO products (id, name, category, price) VALUES
    (1, 'Apple', 'fruit', 1.50),
    (2, 'Banana', 'fruit', 0.75),
    (3, 'Carrot', 'vegetable', 2.00),
    (4, 'Broccoli', 'vegetable', 3.25),
    (5, 'Milk', 'dairy', 4.50);
