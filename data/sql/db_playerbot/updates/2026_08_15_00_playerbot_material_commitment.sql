-- Durable central authority for bounded material commitments.
--
-- The ledger records capacity certificates. Physical sources must revalidate before
-- irreversible execution. The singleton book row distinguishes an empty ledger from
-- unavailable persistence.
--
-- Rollback after all consumers and commitments are disabled:
--   DROP TABLE IF EXISTS playerbot_economy_material_operation_commitment;
--   DROP TABLE IF EXISTS playerbot_economy_material_reservation;
--   DROP TABLE IF EXISTS playerbot_economy_material_commitment;
--   DROP TABLE IF EXISTS playerbot_economy_material_requirement;
--   DROP TABLE IF EXISTS playerbot_economy_material_intent;
--   DROP TABLE IF EXISTS playerbot_economy_material_operation;
--   DROP TABLE IF EXISTS playerbot_economy_material_book;

CREATE TABLE IF NOT EXISTS playerbot_economy_material_book (
    singleton_id TINYINT UNSIGNED NOT NULL,
    book_revision BIGINT UNSIGNED NOT NULL DEFAULT 0,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (singleton_id),
    CONSTRAINT ck_playerbot_economy_material_book_singleton CHECK (singleton_id = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Singleton revision for the durable material commitment authority';

INSERT INTO playerbot_economy_material_book (singleton_id, book_revision)
VALUES (1, 0)
ON DUPLICATE KEY UPDATE singleton_id = VALUES(singleton_id);

CREATE TABLE IF NOT EXISTS playerbot_economy_material_intent (
    origin_identity VARBINARY(191) NOT NULL,
    owner_kind ENUM(
        'profession_progression', 'stock_maintenance', 'supply_remediation', 'activity_critical', 'group_commitment'
    ) NOT NULL,
    owner_revision BIGINT UNSIGNED NOT NULL,
    market_id INT UNSIGNED NOT NULL,
    bounded_quantity INT UNSIGNED NOT NULL,
    needed_by BIGINT UNSIGNED NULL DEFAULT NULL,
    first_observed_at BIGINT UNSIGNED NOT NULL,
    last_observed_at BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (origin_identity),
    INDEX idx_playerbot_economy_material_intent_horizon (needed_by, first_observed_at),
    CONSTRAINT ck_playerbot_economy_material_intent_owner CHECK (owner_revision > 0),
    CONSTRAINT ck_playerbot_economy_material_intent_market CHECK (market_id > 0),
    CONSTRAINT ck_playerbot_economy_material_intent_quantity CHECK (bounded_quantity > 0),
    CONSTRAINT ck_playerbot_economy_material_intent_observed CHECK (last_observed_at >= first_observed_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Visible latent and horizon-bearing material intents';

CREATE TABLE IF NOT EXISTS playerbot_economy_material_requirement (
    origin_identity VARBINARY(191) NOT NULL,
    requirement_ordinal INT UNSIGNED NOT NULL,
    item_id INT UNSIGNED NOT NULL,
    quantity INT UNSIGNED NOT NULL,
    PRIMARY KEY (origin_identity, requirement_ordinal),
    UNIQUE KEY uk_playerbot_economy_material_requirement_item (origin_identity, item_id),
    CONSTRAINT fk_playerbot_economy_material_requirement_intent FOREIGN KEY (origin_identity)
        REFERENCES playerbot_economy_material_intent (origin_identity) ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT ck_playerbot_economy_material_requirement_item CHECK (item_id > 0),
    CONSTRAINT ck_playerbot_economy_material_requirement_quantity CHECK (quantity > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Exact per item material requirements for each intent';

CREATE TABLE IF NOT EXISTS playerbot_economy_material_commitment (
    public_id CHAR(18) NOT NULL,
    origin_identity VARBINARY(191) NOT NULL,
    owner_kind ENUM(
        'profession_progression', 'stock_maintenance', 'supply_remediation', 'activity_critical', 'group_commitment'
    ) NOT NULL,
    owner_revision BIGINT UNSIGNED NOT NULL,
    market_id INT UNSIGNED NOT NULL,
    material_item_id INT UNSIGNED NOT NULL,
    bounded_quantity INT UNSIGNED NOT NULL,
    remaining_quantity INT UNSIGNED NOT NULL,
    needed_by BIGINT UNSIGNED NOT NULL,
    state ENUM('admitted', 'partially_fulfilled', 'completed', 'released', 'superseded') NOT NULL,
    PRIMARY KEY (public_id),
    INDEX idx_playerbot_economy_material_commitment_origin (origin_identity, state),
    INDEX idx_playerbot_economy_material_commitment_horizon (state, needed_by),
    CONSTRAINT fk_playerbot_economy_material_commitment_intent FOREIGN KEY (origin_identity)
        REFERENCES playerbot_economy_material_intent (origin_identity) ON DELETE RESTRICT ON UPDATE CASCADE,
    CONSTRAINT ck_playerbot_economy_material_commitment_owner CHECK (owner_revision > 0),
    CONSTRAINT ck_playerbot_economy_material_commitment_market CHECK (market_id > 0),
    CONSTRAINT ck_playerbot_economy_material_commitment_item CHECK (material_item_id > 0),
    CONSTRAINT ck_playerbot_economy_material_commitment_quantity CHECK (
        bounded_quantity > 0 AND remaining_quantity <= bounded_quantity
        AND ((state IN ('admitted', 'partially_fulfilled') AND remaining_quantity > 0)
             OR (state IN ('completed', 'released', 'superseded') AND remaining_quantity = 0))
    ),
    CONSTRAINT ck_playerbot_economy_material_commitment_partial CHECK (
        (state = 'admitted' AND remaining_quantity = bounded_quantity)
        OR (state = 'partially_fulfilled' AND remaining_quantity < bounded_quantity)
        OR state IN ('completed', 'released', 'superseded')
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='One durable opaque commitment per admitted material requirement';

CREATE TABLE IF NOT EXISTS playerbot_economy_material_reservation (
    commitment_public_id CHAR(18) NOT NULL,
    reservation_ordinal INT UNSIGNED NOT NULL,
    material_item_id INT UNSIGNED NOT NULL,
    capacity_kind ENUM('owned_item', 'auction_listing', 'money', 'gathering_capacity', 'production_capacity') NOT NULL,
    capacity_identity VARBINARY(191) NOT NULL,
    capacity_unit ENUM('item_units', 'copper', 'gathering_units', 'production_units') NOT NULL,
    authority_revision BIGINT UNSIGNED NOT NULL,
    initial_backed_material_quantity BIGINT UNSIGNED NOT NULL,
    remaining_backed_material_quantity BIGINT UNSIGNED NOT NULL,
    initial_capacity_quantity BIGINT UNSIGNED NOT NULL,
    remaining_capacity_quantity BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (commitment_public_id, reservation_ordinal),
    UNIQUE KEY uk_playerbot_economy_material_reservation_capacity (
        commitment_public_id, capacity_kind, capacity_identity
    ),
    INDEX idx_playerbot_economy_material_reservation_authority (
        capacity_kind, capacity_identity, authority_revision
    ),
    CONSTRAINT fk_playerbot_economy_material_reservation_commitment FOREIGN KEY (commitment_public_id)
        REFERENCES playerbot_economy_material_commitment (public_id) ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT ck_playerbot_economy_material_reservation_item CHECK (material_item_id > 0),
    CONSTRAINT ck_playerbot_economy_material_reservation_authority CHECK (authority_revision > 0),
    CONSTRAINT ck_playerbot_economy_material_reservation_capacity CHECK (
        initial_capacity_quantity > 0 AND remaining_capacity_quantity > 0
        AND remaining_capacity_quantity <= initial_capacity_quantity
    ),
    CONSTRAINT ck_playerbot_economy_material_reservation_backing CHECK (
        remaining_backed_material_quantity <= initial_backed_material_quantity
        AND ((capacity_kind = 'money' AND capacity_unit = 'copper'
              AND initial_backed_material_quantity = 0 AND remaining_backed_material_quantity = 0)
             OR (capacity_kind IN ('owned_item', 'auction_listing')
                 AND initial_backed_material_quantity = initial_capacity_quantity
                 AND remaining_backed_material_quantity = remaining_capacity_quantity)
             OR (capacity_kind IN ('gathering_capacity', 'production_capacity')
                 AND initial_backed_material_quantity > 0
                 AND remaining_backed_material_quantity > 0))
    ),
    CONSTRAINT ck_playerbot_economy_material_reservation_unit CHECK (
        (capacity_kind IN ('owned_item', 'auction_listing') AND capacity_unit = 'item_units')
        OR (capacity_kind = 'money' AND capacity_unit = 'copper')
        OR (capacity_kind = 'gathering_capacity' AND capacity_unit = 'gathering_units')
        OR (capacity_kind = 'production_capacity' AND capacity_unit = 'production_units')
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Typed revisioned capacity certificates reserved by active commitments';

CREATE TABLE IF NOT EXISTS playerbot_economy_material_operation (
    operation_identity VARBINARY(191) NOT NULL,
    book_singleton_id TINYINT UNSIGNED NOT NULL DEFAULT 1,
    fingerprint MEDIUMBLOB NOT NULL,
    resulting_book_revision BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (operation_identity),
    UNIQUE KEY uk_playerbot_economy_material_operation_revision (resulting_book_revision),
    CONSTRAINT fk_playerbot_economy_material_operation_book FOREIGN KEY (book_singleton_id)
        REFERENCES playerbot_economy_material_book (singleton_id) ON DELETE RESTRICT ON UPDATE CASCADE,
    CONSTRAINT ck_playerbot_economy_material_operation_revision CHECK (resulting_book_revision > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Restart safe idempotency receipts for authority mutations';

CREATE TABLE IF NOT EXISTS playerbot_economy_material_operation_commitment (
    operation_identity VARBINARY(191) NOT NULL,
    commitment_ordinal INT UNSIGNED NOT NULL,
    commitment_public_id CHAR(18) NOT NULL,
    PRIMARY KEY (operation_identity, commitment_ordinal),
    CONSTRAINT fk_playerbot_economy_material_operation_receipt FOREIGN KEY (operation_identity)
        REFERENCES playerbot_economy_material_operation (operation_identity) ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT fk_playerbot_economy_material_operation_commitment FOREIGN KEY (commitment_public_id)
        REFERENCES playerbot_economy_material_commitment (public_id) ON DELETE RESTRICT ON UPDATE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Ordered opaque commitment identities returned by admitted operations';
