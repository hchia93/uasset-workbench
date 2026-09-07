# Renames a C++ default subobject on every Blueprint that serialized it, so changing the
# CreateDefaultSubobject name does not leave the old export behind as an orphan component.
#
# Order matters: run this FIRST, then change the C++ name and rebuild. Doing it the other way
# round makes the engine build a fresh subobject under the new name while the serialized export
# stays as an orphan, and both end up live on the actor.
#
# Usage:
#   UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript \
#       -script="<this file>" -spec="C:/path/spec.json" [-apply] -unattended -nosplash -nopause
#
# Spec:
#   { "Targets": [ { "NativeClass": "/Script/Module.MyActor",
#                    "Member": "MyComponent",
#                    "OldName": "Old",
#                    "NewName": "New" } ] }
#
# Report lands in <Project>/Intermediate/RenameDefaultSubobject/report.txt

import json
import os
import re
import unreal

SPEC_FLAG = re.compile(r'-spec="?([^"\s]+)"?', re.IGNORECASE)
REQUIRED_KEYS = ("NativeClass", "Member", "OldName", "NewName")


def command_line():
    return unreal.SystemLibrary.get_command_line()


def spec_path():
    match = SPEC_FLAG.search(command_line())
    if not match:
        raise ValueError("no -spec= on the command line")
    return match.group(1)


def apply_requested():
    return "-apply" in command_line().lower()


def report_path():
    directory = os.path.join(unreal.Paths.project_intermediate_dir(), "RenameDefaultSubobject")
    if not os.path.isdir(directory):
        os.makedirs(directory)
    return os.path.join(directory, "report.txt")


def read_spec(path):
    with open(path, "r", encoding="utf-8") as handle:
        spec = json.load(handle)
    targets = spec.get("Targets")
    if not targets:
        raise ValueError("spec has no Targets")
    for target in targets:
        missing = [key for key in REQUIRED_KEYS if not target.get(key)]
        if missing:
            raise ValueError("target %s is missing %s" % (target, ", ".join(missing)))
    return targets


def blueprints_by_native_parent():
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_class(unreal.TopLevelAssetPath("/Script/Engine", "Blueprint"), True)
    grouped = {}
    for data in assets:
        raw = data.get_tag_value("NativeParentClass")
        if not raw:
            continue
        raw = str(raw)
        if "'" in raw:
            raw = raw.split("'")[1]
        raw = raw.rstrip('"').strip()
        grouped.setdefault(raw, []).append(str(data.package_name))
    return grouped


def derived_packages(grouped, native_class):
    target_class = unreal.load_object(None, native_class)
    if target_class is None:
        raise ValueError("cannot load %s" % native_class)
    target_cdo = unreal.get_default_object(target_class)
    packages = []
    for parent_path, package_list in grouped.items():
        parent = unreal.load_object(None, parent_path)
        if parent is None:
            continue
        try:
            parent_cdo = unreal.get_default_object(parent)
        except Exception:
            continue
        if isinstance(parent_cdo, type(target_cdo)):
            packages.extend(package_list)
    return sorted(set(packages))


def run():
    lines = []
    apply_mode = apply_requested()
    lines.append("mode: %s" % ("APPLY" if apply_mode else "DRY RUN"))
    lines.append("")

    targets = read_spec(spec_path())
    grouped = blueprints_by_native_parent()

    renamed = 0
    skipped = 0
    failed = 0
    touched = set()

    for target in targets:
        native_class = target["NativeClass"]
        member = target["Member"]
        old_name = target["OldName"]
        new_name = target["NewName"]
        lines.append('### %s :: %s   "%s" -> "%s"' % (native_class, member, old_name, new_name))

        for package in derived_packages(grouped, native_class):
            try:
                blueprint = unreal.load_asset(package)
                cdo = unreal.get_default_object(blueprint.generated_class())
                subobject = cdo.get_editor_property(member)
            except Exception as error:
                lines.append("  FAIL   %s  (%s)" % (package, error))
                failed += 1
                continue

            if subobject is None:
                lines.append("  skip   %s  (member is None)" % package)
                skipped += 1
                continue

            path = subobject.get_path_name()
            if not path.startswith(package + "."):
                lines.append("  skip   %s  (inherited, lives at %s)" % (package, path))
                skipped += 1
                continue

            if subobject.get_name() != old_name:
                lines.append('  skip   %s  (already named "%s")' % (package, subobject.get_name()))
                skipped += 1
                continue

            if not apply_mode:
                lines.append("  would rename  %s  ->  %s" % (path, new_name))
                renamed += 1
                continue

            try:
                subobject.rename(new_name)
                after = cdo.get_editor_property(member)
                if after.get_name() != new_name:
                    lines.append('  FAIL   %s  (read back "%s")' % (package, after.get_name()))
                    failed += 1
                    continue
                lines.append("  ok     %s  ->  %s" % (package, after.get_path_name()))
                renamed += 1
                touched.add(package)
            except Exception as error:
                lines.append("  FAIL   %s  (%s)" % (package, error))
                failed += 1

        lines.append("")

    if apply_mode and touched:
        lines.append("=== save ===")
        for package in sorted(touched):
            try:
                saved = unreal.EditorAssetLibrary.save_asset(package, only_if_is_dirty=False)
                lines.append("  save %s -> %s" % (package, saved))
                if not saved:
                    failed += 1
            except Exception as error:
                lines.append("  FAIL save %s (%s)" % (package, error))
                failed += 1
        lines.append("")

    lines.append("renamed: %d   skipped: %d   failed: %d" % (renamed, skipped, failed))
    if not apply_mode:
        lines.append("dry run, nothing written. add -apply to commit.")

    destination = report_path()
    with open(destination, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))

    for line in lines:
        unreal.log(line)

    if failed:
        unreal.log_error("rename_default_subobject: %d failure(s), see %s" % (failed, destination))


run()
