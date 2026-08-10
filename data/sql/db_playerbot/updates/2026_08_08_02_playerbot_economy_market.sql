-- Persistent market evidence, speculative positions, circulation provenance, and cooldowns.
--
-- Runtime decisions load these tables into validated memory at startup. Runtime writes use
-- asynchronous prepared statements. No table stores a copied item or gold balance.
--
-- Rollback for a disabled and fully reconciled economy:
--   DROP TABLE IF EXISTS playerbot_economy_circulation;
--   DROP TABLE IF EXISTS playerbot_economy_price_evidence;
--   DROP TABLE IF EXISTS playerbot_economy_cooldown;
--   DROP TABLE IF EXISTS playerbot_economy_position;

CREATE TABLE IF NOT EXISTS playerbot_economy_position (
    public_id CHAR(20) NOT NULL,
    trader_guid INT UNSIGNED NOT NULL,
    market_id INT UNSIGNED NOT NULL,
    item_id INT UNSIGNED NOT NULL,
    substitution_group VARCHAR(64) NOT NULL,
    initial_quantity INT UNSIGNED NOT NULL,
    remaining_quantity INT UNSIGNED NOT NULL,
    acquisition_cost BIGINT UNSIGNED NOT NULL,
    realized_cost BIGINT UNSIGNED NOT NULL DEFAULT 0,
    realized_proceeds BIGINT UNSIGNED NOT NULL DEFAULT 0,
    realized_fees BIGINT UNSIGNED NOT NULL DEFAULT 0,
    state ENUM('pending', 'open', 'listed', 'closed', 'lost') NOT NULL,
    relist_attempts TINYINT UNSIGNED NOT NULL DEFAULT 0,
    maximum_relist_attempts TINYINT UNSIGNED NOT NULL,
    cooldown_seconds INT UNSIGNED NOT NULL,
    opened_at TIMESTAMP NOT NULL,
    holding_deadline TIMESTAMP NOT NULL,
    updated_at TIMESTAMP NOT NULL,
    closed_at TIMESTAMP NULL DEFAULT NULL,
    realized_outcome ENUM('sale', 'use', 'transformation', 'vendor', 'loss') NULL DEFAULT NULL,
    PRIMARY KEY (public_id),
    INDEX idx_playerbot_economy_position_market_state (market_id, state, opened_at),
    INDEX idx_playerbot_economy_position_trader_state (trader_guid, state),
    CONSTRAINT ck_playerbot_economy_position_initial CHECK (initial_quantity > 0),
    CONSTRAINT ck_playerbot_economy_position_remaining CHECK (
        remaining_quantity <= initial_quantity
        AND ((state IN ('pending', 'open', 'listed') AND remaining_quantity > 0) OR remaining_quantity = 0)
    ),
    CONSTRAINT ck_playerbot_economy_position_cost CHECK (
        (state IN ('pending', 'open', 'listed') AND acquisition_cost > 0)
        OR (state IN ('closed', 'lost') AND acquisition_cost = 0)
    ),
    CONSTRAINT ck_playerbot_economy_position_relist CHECK (
        maximum_relist_attempts > 0 AND relist_attempts <= maximum_relist_attempts
    ),
    CONSTRAINT ck_playerbot_economy_position_cooldown CHECK (cooldown_seconds > 0),
    CONSTRAINT ck_playerbot_economy_position_horizon CHECK (
        holding_deadline > opened_at AND updated_at >= opened_at
        AND (closed_at IS NULL OR closed_at >= opened_at)
    ),
    CONSTRAINT ck_playerbot_economy_position_closed CHECK (
        (state IN ('pending', 'open', 'listed') AND closed_at IS NULL AND realized_outcome IS NULL)
        OR (state IN ('closed', 'lost') AND closed_at IS NOT NULL AND realized_outcome IS NOT NULL)
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Restart safe quantity level speculative positions';

CREATE TABLE IF NOT EXISTS playerbot_economy_price_evidence (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    market_id INT UNSIGNED NOT NULL,
    item_id INT UNSIGNED NOT NULL,
    substitution_group VARCHAR(64) NOT NULL,
    source ENUM('sale', 'listing', 'recovery', 'speculation') NOT NULL,
    auction_id INT UNSIGNED NULL DEFAULT NULL,
    unit_price BIGINT UNSIGNED NOT NULL,
    quantity INT UNSIGNED NOT NULL,
    observed_at TIMESTAMP NOT NULL,
    expires_at TIMESTAMP NOT NULL,
    position_public_id CHAR(20) NULL DEFAULT NULL,
    PRIMARY KEY (id),
    INDEX idx_playerbot_economy_evidence_market_item (market_id, item_id, observed_at),
    INDEX idx_playerbot_economy_evidence_group (market_id, substitution_group, observed_at),
    INDEX idx_playerbot_economy_evidence_expiry (expires_at),
    UNIQUE KEY uk_playerbot_economy_evidence_auction (market_id, auction_id, source),
    CONSTRAINT fk_playerbot_economy_evidence_position FOREIGN KEY (position_public_id)
        REFERENCES playerbot_economy_position (public_id) ON DELETE SET NULL ON UPDATE CASCADE,
    CONSTRAINT ck_playerbot_economy_evidence_price CHECK (unit_price > 0),
    CONSTRAINT ck_playerbot_economy_evidence_quantity CHECK (quantity > 0),
    CONSTRAINT ck_playerbot_economy_evidence_expiry CHECK (expires_at > observed_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Bounded completed sale and listing observations';

CREATE TABLE IF NOT EXISTS playerbot_economy_circulation (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    position_public_id CHAR(20) NOT NULL,
    item_guid BIGINT UNSIGNED NOT NULL,
    quantity INT UNSIGNED NOT NULL,
    auction_id INT UNSIGNED NULL DEFAULT NULL,
    provenance ENUM('ordinary', 'speculative', 'recovery') NOT NULL,
    state ENUM(
        'pending', 'acquired', 'listed', 'delivered', 'merged', 'consumed', 'transformed', 'vendored', 'lost'
    ) NOT NULL,
    occurred_at TIMESTAMP NOT NULL,
    PRIMARY KEY (id),
    INDEX idx_playerbot_economy_circulation_position (position_public_id, occurred_at),
    INDEX idx_playerbot_economy_circulation_item (item_guid, occurred_at),
    INDEX idx_playerbot_economy_circulation_auction (auction_id, provenance, occurred_at),
    CONSTRAINT fk_playerbot_economy_circulation_position FOREIGN KEY (position_public_id)
        REFERENCES playerbot_economy_position (public_id) ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT ck_playerbot_economy_circulation_quantity CHECK (quantity > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Immutable provenance events for speculative quantities';

CREATE TABLE IF NOT EXISTS playerbot_economy_cooldown (
    trader_guid INT UNSIGNED NOT NULL,
    market_id INT UNSIGNED NOT NULL,
    substitution_group VARCHAR(64) NOT NULL,
    cause ENUM('loss', 'failed_purchase', 'failed_listing', 'expired') NOT NULL,
    next_eligible_at TIMESTAMP NOT NULL,
    PRIMARY KEY (trader_guid, market_id, substitution_group),
    INDEX idx_playerbot_economy_cooldown_expiry (next_eligible_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Per trader and substitution group market making cooldowns';
