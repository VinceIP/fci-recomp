"""The game_driver.py steps that write to the control file (RECOMP_FCI_CONTROL): post (a game
command) and checkpoint (Story Resume's command), the lines the runtime posts at the frame
boundary (src/fci_threads.c, src/xbr/fci_quicksave.cpp)."""

import pytest

import game_driver


def test_post_step_writes_the_command_with_tilde_as_space():
    assert game_driver.control_line("post:SaveGame~checkpoint=Quick_Bunker") == (
        "post SaveGame checkpoint=Quick_Bunker"
    )


def test_post_step_replaces_every_tilde():
    spec = "post:StartLevel~level=The_Bunker~mission=The_Bunker~checkpoint=Quick_The_Bunker"
    assert game_driver.control_line(spec) == (
        "post StartLevel level=The_Bunker mission=The_Bunker checkpoint=Quick_The_Bunker"
    )


def test_checkpoint_step_posts_the_resume_command():
    # level and mission are the level; the checkpoint is the save's name
    assert game_driver.control_line("checkpoint:The_Bunker:Quick_The_Bunker") == (
        "post StartLevel level=The_Bunker mission=The_Bunker checkpoint=Quick_The_Bunker"
    )


@pytest.mark.parametrize("spec", ["checkpoint:", "checkpoint:The_Bunker", "checkpoint:The_Bunker:"])
def test_checkpoint_step_wants_level_and_name(spec):
    with pytest.raises(game_driver.StepFailed, match="LEVEL:NAME"):
        game_driver.control_line(spec)


def test_control_line_refuses_steps_that_are_not_control_steps():
    with pytest.raises(game_driver.StepFailed, match="unknown control step"):
        game_driver.control_line("mouse:1:2")
    with pytest.raises(game_driver.StepFailed, match="unknown control step"):
        game_driver.control_line("capture")


def test_same_frames_are_pairs_of_captured_frames():
    # regress.check unpacks each entry as (a, b); a flat list once crashed the quicksave run
    import regress

    for name, sc in regress.SCENARIOS.items():
        captures = sum(1 for s in sc.get("steps", []) if s == "capture")
        for pair in sc.get("same_frames", []):
            a, b = pair
            assert 0 <= a < captures and 0 <= b < captures, f"{name}: {pair} outside {captures} captures"
