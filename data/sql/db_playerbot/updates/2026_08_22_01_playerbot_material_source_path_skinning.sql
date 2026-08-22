-- A gathering source path may also be skinning (393): the catalog models skinnable creature
-- populations as nodes, so a leatherworking progression sources its own leather the same way herbs
-- and ore are sourced. The previous check listed herbalism and mining only, and the first skinning
-- path insert failed the constraint, which turned the material commitment authority read only.
ALTER TABLE playerbot_economy_material_source_path
    DROP CHECK ck_playerbot_economy_material_source_skill;
ALTER TABLE playerbot_economy_material_source_path
    ADD CONSTRAINT ck_playerbot_economy_material_source_skill CHECK (
        (source_kind = 'same_actor_gathering' AND gathering_skill_id IN (182, 186, 393))
        OR (source_kind = 'same_actor_hunting' AND gathering_skill_id = 0)
    );
