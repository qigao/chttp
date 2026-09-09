"""Offline checks for the generator's documented OpenAPI 3.1 subset."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

from jsonschema import Draft202012Validator, SchemaError, ValidationError
import yaml

SCHEMA_SHA256 = "da01ba28852cac0de53893797cb8d1942bc3b05084f526dcc216717dec314ed0"
PROCESS_TIMEOUT = 30
METHODS = ("get", "put", "post", "delete", "options", "head", "patch", "trace")
# Each tuple supplies annotation input and independently chosen valid/invalid instances.
CASES = [
    ("minimum", "number", "@minimum body 2", [2, 3], [1]),
    ("maximum", "number", "@maximum body 2", [1, 2], [3]),
    ("exclusiveMinimum", "number", "@exclusiveMinimum body 2", [3], [1, 2]),
    ("exclusiveMaximum", "number", "@exclusiveMaximum body 2", [1], [2, 3]),
    ("multipleOf", "number", "@multipleOf body 0.5", [0, 1.5], [1.25]),
    ("minLength", "string", "@minLength body 2", ["ab", "猫狗"], ["", "猫"]),
    ("maxLength", "string", "@maxLength body 2", ["", "猫狗"], ["abc"]),
    ("pattern", "string", "@pattern body ^[a-z]+$", ["pet"], ["", "Pet", "123"]),
    ("format", "string", "@format body email", ["a@b.example", "annotation-only"], []),
    ("minItems", "int[]", "@minItems body 2", [[1, 2]], [[], [1]]),
    ("maxItems", "int[]", "@maxItems body 2", [[], [1, 2]], [[1, 2, 3]]),
    ("uniqueItems", "int[]", "@uniqueItems body true", [[], [1, 2]], [[1, 1]]),
    ("minProperties", "object", "@minProperties body 1", [{"a": 1}], [{}]),
    ("maxProperties", "object", "@maxProperties body 1", [{}, {"a": 1}], [{"a": 1, "b": 2}]),
    ("additionalProperties", "object", "@field a int optional A\n@additionalProperties body false", [{}, {"a": 1}], [{"b": 2}]),
    ("enum", "string", "@enum body cat\n@enum body dog", ["cat", "dog"], ["CAT", "bird"]),
    ("required", "object", "@field name string required Name", [{"name": ""}], [{}, {"name": None}]),
    ("items", "string[]", "@minLength body[] 2", [[], ["ab"]], [["a"], [1]]),
    # An unsatisfiable schema is legal JSON Schema, not a malformed document.
    ("emptyRange", "number", "@minimum body 3\n@maximum body 1", [], [0, 2, 4]),
    ("exactInteger", "number", "@minimum body 9007199254740993", [9007199254740993], [9007199254740992]),
    ("exactEnum", "number", "@enum body 9007199254740993", [9007199254740993], [9007199254740992, 9007199254740994]),
    ("exponent", "number", "@minimum body 1e3", [1000], [999]),
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def run(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, text=True,
                          encoding="utf-8", timeout=PROCESS_TIMEOUT, **kwargs)


def schemas(node):
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "schema":
                yield value
            else:
                yield from schemas(value)
    elif isinstance(node, list):
        for value in node:
            yield from schemas(value)


def patterns(node):
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "pattern":
                yield value
            elif key in ("properties", "$defs", "patternProperties"):
                for child in value.values():
                    yield from patterns(child)
            elif key in ("items", "additionalProperties", "contains", "not", "if", "then", "else"):
                yield from patterns(value)
            elif key in ("allOf", "anyOf", "oneOf", "prefixItems"):
                for child in value:
                    yield from patterns(child)


def check_document(document, node):
    raw = (Path(__file__).parent / "schema/openapi-3.1-2022-10-07.json").read_bytes()
    require(hashlib.sha256(raw).hexdigest() == SCHEMA_SHA256, "official schema checksum mismatch")
    Draft202012Validator(json.loads(raw)).validate(document)
    ids, templates = set(), set()
    for path, item in document.get("paths", {}).items():
        names = re.findall(r"\{([^{}\/]+)\}", path)
        canonical = re.sub(r"\{[^{}\/]+\}", "{}", path)
        require("{" not in canonical.replace("{}", "") and "}" not in canonical.replace("{}", ""), "malformed path template")
        require(canonical not in templates, "equivalent path templates")
        templates.add(canonical)
        for method in METHODS:
            if method not in item:
                continue
            operation = item[method]
            operation_id = operation.get("operationId")
            if operation_id is not None:
                require(operation_id not in ids, "duplicate operationId")
                ids.add(operation_id)
            params = operation.get("parameters", [])
            for parameter in params:
                if parameter["in"] in ("header", "cookie"):
                    require(re.fullmatch(r"[!#$%&'*+.^_`|~0-9A-Za-z-]+", parameter["name"]) is not None,
                            "invalid header/cookie parameter name")
            identities = [(p["in"], p["name"]) for p in params]
            require(len(set(identities)) == len(identities), "duplicate parameters")
            path_params = {p["name"] for p in params if p["in"] == "path"}
            require(set(names) == path_params, "path parameter mismatch")
    all_schemas = list(schemas(document))
    all_patterns = []
    for schema in all_schemas:
        # Python regex syntax is not the ECMA-262 dialect used by OpenAPI.
        Draft202012Validator.check_schema(schema, format_checker=None)
        all_patterns.extend(patterns(schema))
    run([node, "-e", "const fs=require('fs'); for(const p of JSON.parse(fs.readFileSync(0,'utf8'))) new RegExp(p);"],
        input=json.dumps(all_patterns))
    return len(all_schemas), len(all_patterns)


def annotation(tags, name):
    return f"/**\n{tags}\n*/\nint case_{name}(void) {{ return 1; }}\n"


def require_rejected(document, node, label):
    try:
        check_document(document, node)
    except (ValueError, SchemaError, ValidationError, subprocess.CalledProcessError):
        return
    raise ValueError(f"invalid {label} escaped conformance checking")


def suite(generator, plugin, node):
    source = []
    for name, type_name, tags, _, _ in CASES:
        source.append(annotation(f"@route POST /{name}\n@body application/json {type_name} required Value\n{tags}\n@response 200 OK", name))
    for method in METHODS:
        source.append(annotation(f"@route {method.upper()} /pets/{{id}}\n@param id path int required Id\n@param limit query integer optional Limit\n@minimum query.limit 1\n@param key header string optional Key\n@maxLength header.key 80\n@param key cookie string optional Cookie\n@minLength cookie.key 1\n@summary Summary\n@description Description\n@tag pets\n@deprecated false\n@operationId pets_{method}\n@response 2XX Success\n@produces 2XX application/json string[]\n@response default Error", f"pets_{method}"))
    for index, alias in enumerate(("char*", "bool", "float", "double", "integer", "boolean", "object[][]")):
        source.append(annotation(f"@route POST /alias{index}\n@body application/json {alias} optional Value\n@response 201 Created", f"alias{index}"))
    source.append(annotation("@route GET /regex\n@body application/json string optional Text\n@pattern body (?<word>[a-z]+)\n@response 200 OK", "regex_ecma"))
    # Concrete routes coexist with templates, and matching braces may repeat a name.
    source.append(annotation("@route GET /pets/mine\n@brief Mine\n@details Detail\n@response 200 OK", "mine"))
    source.append(annotation("@route GET /repeat/{id}/{id}\n@param id path int required Id\n@response 200 OK", "repeat"))
    source.append(annotation("@route POST /body-scope\n@param body query number optional Query\n@minimum query.body 1\n@body application/json number required Body\n@minimum body 10\n@response 200 OK", "body_scope"))
    media_types = ("application/vnd.api+json", "text/*", 'text/plain;charset="utf-8";profile="a;b"')
    for index, media in enumerate(media_types):
        source.append(annotation(f"@route POST /media{index}\n@body {media} string required Body\n@response 200 OK\n@produces 200 {media} string", f"media{index}"))
    with tempfile.TemporaryDirectory(prefix="openapi-conformance-") as directory:
        path = Path(directory) / "cases.c"
        path.write_text("".join(source), encoding="utf-8")
        command = [generator, "--plugin", plugin]
        document = json.loads(run(command + ["--format", "json", str(path)]).stdout)
        yaml_document = yaml.safe_load(run(command + ["--format", "yaml", str(path)]).stdout)
        require(document == yaml_document, "JSON/YAML semantic mismatch")
        count, regex_count = check_document(document, node)
        for index, media in enumerate(media_types):
            operation = document["paths"][f"/media{index}"]["post"]
            require(list(operation["requestBody"]["content"]) == [media], "request media key changed")
            require(list(operation["responses"]["200"]["content"]) == [media], "response media key changed")
        scoped = document["paths"]["/body-scope"]["post"]
        require(scoped["parameters"][0]["schema"]["minimum"] == 1, "query.body bound to wrong schema")
        require(scoped["requestBody"]["content"]["application/json"]["schema"]["minimum"] == 10, "body bound to wrong schema")
        instances = 0
        for name, _, _, valid, invalid in CASES:
            schema = document["paths"][f"/{name}"]["post"]["requestBody"]["content"]["application/json"]["schema"]
            validator = Draft202012Validator(schema)
            for value in valid:
                require(validator.is_valid(value), f"{name}: rejected valid instance {value!r}")
            for value in invalid:
                require(not validator.is_valid(value), f"{name}: accepted invalid instance {value!r}")
            instances += len(valid) + len(invalid)
        # Prove the independent checker catches malformed ECMA-262, not just valid examples.
        broken = json.loads(json.dumps(document))
        broken["paths"]["/regex"]["get"]["requestBody"]["content"]["application/json"]["schema"]["pattern"] = "["
        require_rejected(broken, node, "regex")
        broken = json.loads(json.dumps(document))
        broken["paths"]["/exclusiveMinimum"]["post"]["requestBody"]["content"]["application/json"]["schema"]["exclusiveMinimum"] = True
        require_rejected(broken, node, "3.0-style exclusiveMinimum")
        broken = json.loads(json.dumps(document))
        del broken["paths"]["/pets/{id}"]["get"]["parameters"][0]
        require_rejected(broken, node, "missing path parameter")
        broken = json.loads(json.dumps(document))
        broken["paths"]["/pets/{name}"] = {}
        require_rejected(broken, node, "equivalent path")
        broken = json.loads(json.dumps(document))
        del broken["paths"]["/pets/{id}"]["get"]["responses"]["2XX"]["description"]
        require_rejected(broken, node, "missing response description")
        for location in ("header", "cookie"):
            broken = json.loads(json.dumps(document))
            parameter = next(p for p in broken["paths"]["/pets/{id}"]["get"]["parameters"] if p["in"] == location)
            parameter["name"] = "bad:name"
            require_rejected(broken, node, f"invalid {location} name")
        # The file-check entry point must detect invalid regex emitted from real annotation input.
        path.write_text(annotation("@route POST /bad-regex\n@body application/json string required Value\n@pattern body [\n@response 200 OK", "bad_regex"), encoding="utf-8")
        emitted = json.loads(run(command + [str(path)]).stdout)
        require_rejected(emitted, node, "emitted invalid regex")
    print(f"OpenAPI subset: {len(source)} operations, {count} schemas, {regex_count} ECMA patterns, {instances} boundary instances passed (JSON/YAML).")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--generator")
    inputs.add_argument("--document", type=Path, help="validate a JSON/YAML document from this generator's supported subset")
    parser.add_argument("--plugin")
    parser.add_argument("--node", default="node")
    args = parser.parse_args()
    if args.document:
        text = args.document.read_text(encoding="utf-8")
        document = json.loads(text) if args.document.suffix.lower() == ".json" else yaml.safe_load(text)
        count, regex_count = check_document(document, args.node)
        print(f"Generated document passed: {count} schemas, {regex_count} ECMA patterns.")
    else:
        if not args.plugin:
            parser.error("--generator requires --plugin")
        suite(str(Path(args.generator).resolve()), str(Path(args.plugin).resolve()), args.node)
