#!/usr/bin/env python3
"""stringtable.py — insert rows into CryEngine string table XML files.

String tables are XML files with a structure like:

  <stringtable>
    <section name="P_Main">
      <string enum="B_StoryMode" value="STORY MODE"/>
      <string enum="B_Multiplayer" value="MULTIPLAYER"/>
      ...
    </section>
  </stringtable>

This module provides a function to insert a new <string> row after a
given enum, preserving the file's line endings (CRLF) and indentation.
"""

import re
import sys


def insert_string_row(xml_text: str, section: str, after_enum: str, new_enum: str, new_value: str) -> str:
    """Insert a new <string> row after the row with enum=after_enum in the
    given section.

    Returns the modified XML text.

    Raises ValueError if the section or after_enum is not found.
    """
    # Detect line ending
    if "\r\n" in xml_text:
        newline = "\r\n"
    else:
        newline = "\n"

    # Find the section
    section_pattern = re.compile(r'(<section\s+name="' + re.escape(section) + r'">\s*)(.*?)(</section>)', re.DOTALL)
    section_match = section_pattern.search(xml_text)
    if not section_match:
        raise ValueError(f"section '{section}' not found")

    section_start = section_match.start(2)
    section_end = section_match.end(2)
    section_content = xml_text[section_start:section_end]

    # Find the after_enum row
    # Match: <string enum="after_enum" value="..."/>
    # The row may have varying whitespace
    after_pattern = re.compile(r'([ \t]*<string\s+enum="' + re.escape(after_enum) + r'"\s+value="[^"]*"\s*/>)')
    after_match = after_pattern.search(section_content)
    if not after_match:
        raise ValueError(f"enum '{after_enum}' not found in section '{section}'")

    # Determine the indentation from the after_enum row
    indent = after_match.group(1)[: len(after_match.group(1)) - len(after_match.group(1).lstrip())]

    # Create the new row
    new_row = f'{indent}<string enum="{new_enum}" value="{new_value}"/>'

    # Insert after the after_enum row
    insert_pos = section_start + after_match.end(1)
    # Add newline before the new row
    result = xml_text[:insert_pos] + newline + new_row + xml_text[insert_pos:]

    return result


def rename_string_row(xml_text: str, section: str, old_enum: str, new_enum: str, new_value: str) -> str:
    """Rename a <string> row: change its enum and value in the given section.

    For example, old_enum="B_CheatsDebug", new_enum="B_Quit", new_value="QUIT"
    changes the row from <string enum="B_CheatsDebug" value="CHEATS (DEBUG)"/>
    to <string enum="B_Quit" value="QUIT"/>.

    Returns the modified XML text.
    Raises ValueError if the section or old_enum is not found.
    """
    # Find the section
    section_pattern = re.compile(r'(<section\s+name="' + re.escape(section) + r'">\s*)(.*?)(</section>)', re.DOTALL)
    section_match = section_pattern.search(xml_text)
    if not section_match:
        raise ValueError(f"section '{section}' not found")

    section_start = section_match.start(2)
    section_content = xml_text[section_start : section_match.end(2)]

    # Find the row with the old enum
    row_pattern = re.compile(r'([ \t]*<string\s+enum="' + re.escape(old_enum) + r'"\s+value=")([^"]*)("\s*/>)')
    row_match = row_pattern.search(section_content)
    if not row_match:
        raise ValueError(f"enum '{old_enum}' not found in section '{section}'")

    old_row = row_match.group(0)
    new_row = old_row.replace(f'enum="{old_enum}"', f'enum="{new_enum}"').replace(
        f'value="{row_match.group(2)}"', f'value="{new_value}"'
    )

    result = xml_text[: section_start + row_match.start()] + new_row + xml_text[section_start + row_match.end() :]
    return result


def count_strings(xml_text: str, section: str) -> int:
    """Count the number of <string> rows in a section."""
    section_pattern = re.compile(r'<section\s+name="' + re.escape(section) + r'">\s*(.*?)</section>', re.DOTALL)
    m = section_pattern.search(xml_text)
    if not m:
        return 0
    return len(re.findall(r'<string\s+enum="[^"]*"', m.group(1)))


if __name__ == "__main__":
    # Simple CLI for testing
    if len(sys.argv) < 6:
        print(f"Usage: {sys.argv[0]} <file.xml> <section> <after_enum> <new_enum> <new_value>")
        sys.exit(1)

    filepath, section, after_enum, new_enum, new_value = sys.argv[1:6]
    with open(filepath, "r", encoding="utf-8") as f:
        text = f.read()

    result = insert_string_row(text, section, after_enum, new_enum, new_value)
    with open(filepath, "w", encoding="utf-8", newline="") as f:
        f.write(result)

    print(f"Inserted {new_enum}={new_value!r} after {after_enum} in {section}")
    print(f"Section now has {count_strings(result, section)} strings")
