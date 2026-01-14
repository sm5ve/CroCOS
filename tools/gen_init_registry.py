#!/usr/bin/env python3

import argparse
import tomllib
import warnings
from toollib import *

parser = argparse.ArgumentParser(description='Generate kernel initialization registry')

parser.add_argument('-o', type=file_path,
                    help='Output path')
parser.add_argument('def_files', type=file_path, nargs='+', help="[Input files]")

args = parser.parse_args()

output_path = args.o
input_files = args.def_files

phases_in_order = ["cpp_init", "processor_early", "memory_management", "core_devices", "smp_bringup"]
phase_names = ["C++ Initialization", "Early Processor Bringup", "Memory Management", "Core Devices", "SMP Bringup"]
unique_capability_set = set(["logical_cpu_id"])

phases_to_number = {key: value for value, key in enumerate(phases_in_order)}

components = dict()
components_by_capabilities = dict()

class DuplicateComponentException(Exception):
    def __init__(self, message):
        self.message = message
        super().__init__(self.message)

class MalformedComponentException(Exception):
    def __init__(self, message):
        self.message = message
        super().__init__(self.message)

required_keys = set(["name", "required", "per_cpu", "phase"])
optional_keys = set(["routine", "bootstrap_routine", "ap_routine", "provides_capabilities", "depends_on", "depends_on_capabilities", "logging_importance", "ap_depends_on_capabilities"])

valid_logging_importances = set(["DEBUG", "IMPORTANT", "CRITICAL", "ERROR"])

valid_keys = required_keys.union(optional_keys)
def validate_and_normalize_component(component, name):
    keySet = set(component.keys())
    if not keySet.issuperset(required_keys):
        raise MalformedComponentException(f'{name} is missing required keys: {required_keys - keySet}')
    if not component["per_cpu"]:
        if "routine" not in keySet:
            raise MalformedComponentException(f'{name} is global but missing routine')
    else:
        if "routine" in keySet:
            if "bootstrap_routine" in keySet or "ap_routine" in keySet:
                raise MalformedComponentException(f'{name} has ambiguous routines')
            else:
                component["bootstrap_routine"] = component["routine"]
                component["ap_routine"] = component["routine"]
                del component["routine"]
        elif "bootstrap_routine" in keySet or "ap_routine" in keySet:
            if not set(["bootstrap_routine", "ap_routine"]).issubset(keySet):
                raise MalformedComponentException(f'{name} is per-cpu but missing ap or bootstrap routine')
    if not keySet.issubset(valid_keys):
        raise MalformedComponentException(f'{name} has invalid keys: {keySet - valid_keys}')
    if component["phase"] not in phases_in_order:
        raise MalformedComponentException(f'{name} has invalid phase: {component["phase"]}')
    if "logging_importance" not in component:
        component["logging_importance"] = "DEBUG"
    elif component["logging_importance"] not in valid_logging_importances:
        raise MalformedComponentException(f'{name} has invalid logging importance: {component["logging_importance"]}')
    if not component["per_cpu"] and "ap_depends_on_capabilities" in component:
        raise MalformedComponentException(f'{name} is global but specifies AP-specific dependencies')
def sort_components_by_capabilities():
    for k, v in components.items():
        if "provides_capabilities" in v:
            for cap in v["provides_capabilities"]:
                if cap in unique_capability_set and cap in components_by_capabilities:
                    raise MalformedComponentException(f'Capability {cap} is defined by multiple components, but must be unique')
                components_by_capabilities.setdefault(cap, []).append(k)

def compute_dependency_info(phase):
    backwards_deps = dict()
    input_valence = dict()
    for k, v in components.items():
        if v["phase"] != phase:
            continue
        forwards_deps = set()
        if "depends_on" in v:
            for dep in v["depends_on"]:
                backwards_deps.setdefault(dep, set()).add(k)
                forwards_deps.add(dep)
        if "depends_on_capabilities" in v:
            for cap in v["depends_on_capabilities"]:
                if cap in components_by_capabilities:
                    for dep in components_by_capabilities[cap]:
                        if components[dep]["phase"] != phase:
                            continue
                        backwards_deps.setdefault(dep, set()).add(k)
                        forwards_deps.add(dep)
                else:
                    warnings.warn(f'Capability {cap} not found in component registry')
        input_valence[k] = len(forwards_deps)
    return backwards_deps, input_valence

def validate_capability_dependency_order():
    stage_last_seen = dict()
    stage_last_seen_definer = dict()
    stage_first_used = dict()
    stage_first_used_definer = dict()
    for k,v in components.items():
        phase = phases_to_number[v["phase"]]
        if "depends_on_capabilities" in v:
            for cap in v["depends_on_capabilities"]:
                if cap not in stage_first_used or stage_first_used[cap] > phase:
                    stage_first_used[cap] = phase
                    stage_first_used_definer[cap] = k
        if "provides_capabilities" in v:
            for cap in v["provides_capabilities"]:
                if cap not in stage_last_seen or stage_last_seen[cap] < phase:
                    stage_last_seen[cap] = phase
                    stage_last_seen_definer[cap] = k
    for k, v in stage_first_used.items():
        if k in stage_last_seen and stage_last_seen[k] > v:
            raise MalformedComponentException(f'Capability {k} is defined by component {stage_last_seen_definer[k]} in a stage after its first use by component {stage_first_used_definer[k]}')
def compute_component_order(phase):
    order = []
    back_deps, input_valence = compute_dependency_info(phase)
    queue = []
    for k, v in input_valence.items():
        if v == 0:
            queue.append(k)
    while len(queue) > 0:
        k = queue.pop(0)
        order.append(k)
        if k in back_deps:
            for dep in back_deps[k]:
                input_valence[dep] -= 1
                if input_valence[dep] == 0:
                    queue.append(dep)
                assert input_valence[dep] >= 0
    for k, v in input_valence.items():
        if v != 0:
            raise MalformedComponentException(f'Component {k} is in dependency cycle')
    return order

for i in input_files:
    with open(i, "rb") as def_file:
        data = tomllib.load(def_file)
        for k, v in data.items():
            if "disabled" in v and v["disabled"]:
                continue
            if k in components:
                raise DuplicateComponentException(k)
            else:
                validate_and_normalize_component(v, k)
                components[k] = v

def validate_dependencies():
    for k, v in components.items():
        if "depends_on" in v:
            for dep in v["depends_on"]:
                if dep not in components:
                    raise MalformedComponentException(f'{k} depends on {dep} which is not defined')

def validate_capabilities_for_required_components():
    for k, v in components.items():
        if v["required"]:
            if "depends_on_capabilities" in v:
                for cap in v["depends_on_capabilities"]:
                    if cap not in components_by_capabilities:
                        raise MalformedComponentException(f'{k} requires capability {cap} which is not defined')

def validate_ap_specific_dependencies():
    globally_initialized_capabilities = set()
    for v in components.values():
        if not v["per_cpu"] and phases_to_number[v["phase"]] < phases_to_number["smp_bringup"]:
            if "provides_capabilities" in v:
                for cap in v["provides_capabilities"]:
                    globally_initialized_capabilities.add(cap)

    for v in components.values():
        if "ap_depends_on_capabilities" in v:
            for cap in v["ap_depends_on_capabilities"]:
                if cap not in globally_initialized_capabilities:
                    raise MalformedComponentException(f'{v["name"]} specifies AP-specific dependencies on capability {cap} which is not globally initialized')

sort_components_by_capabilities()
validate_capability_dependency_order()
validate_dependencies()
validate_capabilities_for_required_components()
validate_ap_specific_dependencies()
order = []
for phase in phases_in_order:
    order.append((phase, compute_component_order(phase)))

ap_id_available = False
def encode_component(component):
    global ap_id_available
    data = components[component]
    flags = []
    if "provides_capabilities" in data and "logical_cpu_id" in data["provides_capabilities"]:
        ap_id_available = True
    elif ap_id_available:
        flags.append("CF_AP_ID_AVAILABLE")
    if data["per_cpu"]:
        routines = f'.bootstrap_initializer = {data["bootstrap_routine"]}, .ap_initializer = {data["ap_routine"]}'
        flags.append("CF_PER_CPU")
    else:
        routines = f'.bootstrap_initializer = {data["routine"]}, .ap_initializer = {data["routine"]}'
    if data["required"]:
        flags.append("CF_REQUIRED")

    flag_str = "CF_NONE" if len(flags) == 0 else " | ".join(flags)
    return f'{{ .name = "{data["name"]}", {routines}, .flags =  {flag_str}, .logging_importance = LoggingImportance::{data["logging_importance"]} }}'
def encode_phase_marker(phase):
    if ap_id_available:
        return f'{{ .name = "{phase_names[phases_to_number[phase]]}", .bootstrap_initializer = nullptr, .ap_initializer = nullptr, .flags = CF_PHASE_MARKER | CF_AP_ID_AVAILABLE, .logging_importance = LoggingImportance::IMPORTANT }}'
    return f'{{ .name = "{phase_names[phases_to_number[phase]]}", .bootstrap_initializer = nullptr, .ap_initializer = nullptr, .flags = CF_PHASE_MARKER, .logging_importance = LoggingImportance::IMPORTANT }}'

def encode_init_forward_decls(methods):
    no_namespace_methods = set()
    namespaced_methods = dict()
    for m in methods:
        parts = m.rsplit("::", 1)
        if len(parts) == 2:
            namespaced_methods.setdefault(parts[0], set()).add(parts[1])
        else:
            no_namespace_methods.add(m)

    out = []
    for nns in no_namespace_methods:
        out.append(f'extern bool {nns}();')
    for k, v in namespaced_methods.items():
        out.append(f'namespace {k} {{')
        out.append("\n".join([f'\textern bool {m}();' for m in v]))
        out.append("}")
        out.append("")

    return "\n".join(out)
def generate_header():
    component_decls = []
    forward_decl_methods = set()
    for phase, cs in order:
        component_decls.append(f"\t\t{encode_phase_marker(phase)}")
        for component in cs:
            component_decls.append(f'\t\t{encode_component(component)}')
            if components[component]["per_cpu"]:
                forward_decl_methods.add(components[component]["bootstrap_routine"])
                forward_decl_methods.add(components[component]["ap_routine"])
            else:
                forward_decl_methods.add(components[component]["routine"])
    component_decls.append("\t\tEND_SENTINEL")

    out = []
    out.append("#include <init.h>")
    out.append(encode_init_forward_decls(forward_decl_methods))
    out.append("namespace kernel::init {")
    out.append("\tconst InitComponent init_components[] = {")
    out.append(",\n".join(component_decls))
    out.append("\t};")
    out.append(f'\tAtomic<bool> complete_components[{len(component_decls)}];')
    out.append("}")
    return "\n".join(out)

with open(output_path, "w") as out_file:
    out_file.write(generate_header())