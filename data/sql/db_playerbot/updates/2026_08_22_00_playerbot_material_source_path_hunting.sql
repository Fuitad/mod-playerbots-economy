-- A material source path may now be a hunt: the actor kills creatures that drop the reagent and loots them.
-- Hunting carries no gathering skill, so the skill check admits 0 for that kind only.
ALTER TABLE playerbot_economy_material_source_path
    DROP CHECK ck_playerbot_economy_material_source_skill;
ALTER TABLE playerbot_economy_material_source_path
    MODIFY COLUMN source_kind ENUM('same_actor_gathering', 'same_actor_hunting') NOT NULL;
ALTER TABLE playerbot_economy_material_source_path
    ADD CONSTRAINT ck_playerbot_economy_material_source_skill CHECK (
        (source_kind = 'same_actor_gathering' AND gathering_skill_id IN (182, 186))
        OR (source_kind = 'same_actor_hunting' AND gathering_skill_id = 0)
    );
