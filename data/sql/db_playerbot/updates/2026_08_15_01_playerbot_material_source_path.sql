CREATE TABLE IF NOT EXISTS playerbot_economy_material_source_path (
    commitment_public_id CHAR(18) NOT NULL,
    source_kind ENUM('same_actor_gathering') NOT NULL,
    phase ENUM('selected', 'acquiring', 'completed', 'released') NOT NULL,
    actor_guid INT UNSIGNED NOT NULL,
    material_item_id INT UNSIGNED NOT NULL,
    selected_quantity INT UNSIGNED NOT NULL,
    gathering_skill_id INT UNSIGNED NOT NULL,
    source_entry INT UNSIGNED NOT NULL,
    source_map_id INT UNSIGNED NOT NULL,
    route_identity VARBINARY(191) NOT NULL,
    capacity_identity VARBINARY(191) NOT NULL,
    source_revision BIGINT UNSIGNED NOT NULL,
    selected_at BIGINT UNSIGNED NOT NULL,
    source_travel_budget_seconds INT UNSIGNED NOT NULL,
    source_action_budget_seconds INT UNSIGNED NOT NULL,
    delivery_travel_budget_seconds INT UNSIGNED NOT NULL,
    completion_observation_budget_seconds INT UNSIGNED NOT NULL,
    destination_yield_basis_points INT UNSIGNED NOT NULL,
    conservative_yield_basis_points INT UNSIGNED NOT NULL,
    observed_gathered_quantity INT UNSIGNED NOT NULL,
    observed_resource_attempts INT UNSIGNED NOT NULL,
    observed_resource_seconds INT UNSIGNED NOT NULL,
    authoritative_interaction_seconds INT UNSIGNED NOT NULL,
    remaining_dedicated_activity_seconds INT UNSIGNED NOT NULL,
    required_resource_count INT UNSIGNED NOT NULL,
    seconds_per_resource INT UNSIGNED NOT NULL,
    starting_inventory_quantity INT UNSIGNED NOT NULL,
    available_resource_count INT UNSIGNED NOT NULL,
    needed_by BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (commitment_public_id),
    INDEX idx_playerbot_economy_material_source_capacity (
        capacity_identity, source_revision, phase
    ),
    INDEX idx_playerbot_economy_material_source_actor (actor_guid, phase, needed_by),
    CONSTRAINT fk_playerbot_economy_material_source_commitment FOREIGN KEY (commitment_public_id)
        REFERENCES playerbot_economy_material_commitment (public_id) ON DELETE CASCADE ON UPDATE CASCADE,
    CONSTRAINT ck_playerbot_economy_material_source_identity CHECK (
        actor_guid > 0 AND material_item_id > 0 AND selected_quantity > 0 AND source_entry > 0
        AND source_revision > 0 AND selected_at > 0
    ),
    CONSTRAINT ck_playerbot_economy_material_source_skill CHECK (gathering_skill_id IN (182, 186)),
    CONSTRAINT ck_playerbot_economy_material_source_horizon CHECK (
        source_action_budget_seconds > 0 AND delivery_travel_budget_seconds = 0
        AND completion_observation_budget_seconds > 0 AND needed_by > selected_at
    ),
    CONSTRAINT ck_playerbot_economy_material_source_yield CHECK (
        destination_yield_basis_points > 0
        AND conservative_yield_basis_points BETWEEN 1 AND destination_yield_basis_points
    ),
    CONSTRAINT ck_playerbot_economy_material_source_capacity CHECK (
        authoritative_interaction_seconds > 0 AND remaining_dedicated_activity_seconds > 0
        AND required_resource_count > 0 AND seconds_per_resource > 0
        AND available_resource_count >= required_resource_count
    ),
    CONSTRAINT ck_playerbot_economy_material_source_history CHECK (
        (observed_resource_attempts = 0 AND observed_gathered_quantity = 0 AND observed_resource_seconds = 0)
        OR (observed_resource_attempts > 0 AND observed_resource_seconds > 0)
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
COMMENT='Durable same actor material delivery path and retained completion baseline';
