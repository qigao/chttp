/**
 * @brief List pets
 * @route GET /pets
 * @details Returns the available **pets**.
 * @param limit query int optional Maximum number of pets to return
 * @minimum limit 1
 * @maximum query.limit 100
 * @response 200 Pet list
 * @produces 200 application/json string[]
 */
int list_pets(int limit) {
    (void)limit;
    return 0;
}

/**
 * @brief Create a pet
 * @route POST /pets
 * @operationId createPet
 * @body application/json object required Pet to create
 * @field name string required Pet name
 * @minLength body.name 1
 * @maxLength body.name 80
 * @additionalProperties body false
 * @response 201 Pet created
 */
int create_pet(const char *name) {
    (void)name;
    return 0;
}
