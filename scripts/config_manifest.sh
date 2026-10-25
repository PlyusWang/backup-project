#!/usr/bin/env bash
manifest_dir() {
  local dir="$1" out="$2"
  local tmp="$out.tmp" list="$out.list" sorted="$out.sorted"
  local files="$out.files" fsorted="$out.fsorted"
  rm -f "$tmp" "$list" "$sorted" "$files" "$fsorted"
  if [ ! -e "$dir" ]; then
    printf 'ABSENT\n' > "$tmp" || return 1
    mv -f "$tmp" "$out" || return 1
    return 2
  fi
  if [ ! -d "$dir" ] || [ ! -r "$dir" ] || [ ! -x "$dir" ]; then
    return 1
  fi
  if ! find "$dir" -printf '%y %P %l\n' > "$list" 2>/dev/null; then
    rm -f "$tmp" "$list" "$sorted" "$files" "$fsorted"
    return 1
  fi
  if ! LC_ALL=C sort "$list" > "$sorted"; then
    rm -f "$tmp" "$list" "$sorted" "$files" "$fsorted"
    return 1
  fi
  if ! find "$dir" -type f -print0 > "$files" 2>/dev/null; then
    rm -f "$tmp" "$list" "$sorted" "$files" "$fsorted"
    return 1
  fi
  if ! LC_ALL=C sort -z "$files" > "$fsorted"; then
    rm -f "$tmp" "$list" "$sorted" "$files" "$fsorted"
    return 1
  fi
  if ! cat "$sorted" > "$tmp"; then
    rm -f "$tmp" "$list" "$sorted" "$files" "$fsorted"
    return 1
  fi
  while IFS= read -r -d '' file; do
    if ! sha256sum -- "$file" >> "$tmp"; then
      rm -f "$tmp" "$list" "$sorted" "$files" "$fsorted"
      return 1
    fi
  done < "$fsorted"
  rm -f "$list" "$sorted" "$files" "$fsorted"
  mv -f "$tmp" "$out" || return 1
  return 0
}
