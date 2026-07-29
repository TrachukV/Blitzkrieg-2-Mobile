package com.nival.blitzkrieg2;

final class OriginalActionButtonCatalog {
    static final class Spec {
        final int action;
        final int slot;
        final boolean ability;
        final String label;
        final String iconPath;

        Spec(
                int action,
                int slot,
                boolean ability,
                String label,
                String iconPath) {
            this.action = action;
            this.slot = slot;
            this.ability = ability;
            this.label = label;
            this.iconPath = iconPath;
        }

        String disabledIconPath() {
            int extension = iconPath.lastIndexOf(".tga");
            if (extension < 0) {
                return iconPath;
            }
            return iconPath.substring(0, extension)
                    + "_D"
                    + iconPath.substring(extension);
        }
    }

    private static final String ROOT =
            "Complete/UI/New_mission/ActionButtons/";

    private OriginalActionButtonCatalog() {
    }

    static Spec forAction(int action) {
        switch (action) {
            case 1:
                return fixed(action, 1, "Move", "MoveTo/MoveTo.tga");
            case 2:
                return fixed(action, 3, "Attack", "Attack/Attack.tga");
            case 4:
                return fixed(action, 7, "Board", "Enter/Enter.tga");
            case 5:
                return fixed(action, 7, "Leave", "Exit/Exit.tga");
            case 6:
                return fixed(action, 6, "Rotate", "RotateTo/RotateTo.tga");
            case 10:
                return ability(action, "Ambush", "Ambush/Ambush.tga");
            case 11:
                return fixed(
                        action,
                        6,
                        "Formations",
                        "ChangeFormation/ChangeFormation.tga");
            case 14:
                return fixed(
                        action,
                        5,
                        "Entrench",
                        "Entrench/Entrench.tga");
            case 15:
                return fixed(
                        action,
                        4,
                        "Stand ground",
                        "StandGround/StandGround.tga");
            case 18:
                return fixed(
                        action,
                        11,
                        "Place mines",
                        "SetMineField/SetMineField.tga");
            case 19:
                return ability(action, "Clear mines", "DeMine/DeMine.tga");
            case 21:
                return ability(
                        action,
                        "Build trenches",
                        "BiildTrenches/BiildTrenches.tga");
            case 22:
                return fixed(action, 7, "Repair", "Repair/Repair.tga");
            case 23:
                return fixed(action, 6, "Resupply", "Resupply/Resupply.tga");
            case 25:
                return fixed(
                        action,
                        5,
                        "Hook artillery",
                        "HookArtillery/HookArtillery.tga");
            case 26:
                return fixed(
                        action,
                        5,
                        "Deploy artillery",
                        "DeployArtillery/DeployArtillery.tga");
            case 39:
                return fixed(action, 2, "Stop", "Stop/Stop.tga");
            case 40:
                return fixed(
                        action,
                        8,
                        "Spyglass",
                        "UseSpyGlass/UseSpyGlass.tga");
            case 41:
                return fixed(action, 7, "Board", "Enter/Enter.tga");
            case 42:
                return fixed(
                        action,
                        12,
                        "Build obstacles",
                        "BuildObstacles/BuildObstacles.tga");
            case 44:
            case 46:
                return ability(
                        action,
                        "Camouflage",
                        "Camouflage/Camouflage.tga");
            case 45:
                return ability(
                        action,
                        "Throw grenade",
                        "ThrowGrenade/ThrowGrenade.tga");
            case 47:
                return ability(
                        action,
                        "Land mine",
                        "SetMineField/SetMineField.tga");
            case 48:
                return ability(
                        action,
                        "Blasting charge",
                        "BlastingCharge/BlastingCharge.tga");
            case 49:
                return ability(
                        action,
                        "Set controlled charge",
                        "ControlledCharge-Set/ControlledCharge-Set.tga");
            case 50:
                return ability(
                        action,
                        "Detonate",
                        "ControlledCharge-Detonate/"
                                + "ControlledCharge-Detonate.tga");
            case 51:
                return ability(
                        action,
                        "Hold sector",
                        "HoldSector/HoldSector.tga");
            case 52:
                return ability(
                        action,
                        "Track targeting",
                        "TrackTargeting/TrackTargeting.tga");
            case 53:
                return ability(
                        action,
                        "Suppressive fire",
                        "SupressiveFire/SupressiveFire.tga");
            case 54:
                return ability(
                        action,
                        "Critical targeting",
                        "CriticalTargeting/CriticalTargeting.tga");
            case 55:
                return ability(
                        action,
                        "Rapid fire",
                        "RapidFire/RapidFire.tga");
            case 56:
                return ability(
                        action,
                        "Cover fire",
                        "CoverFire/CoverFire.tga");
            case 57:
                return ability(
                        action,
                        "Linked grenades",
                        "LinkedGrenades/LinkedGrenades.tga");
            case 58:
                return ability(
                        action,
                        "Support fire",
                        "SupportFire/SupportFire.tga");
            case 59:
                return ability(action, "Patrol", "Patrol/Patrol.tga");
            case 60:
                return ability(action, "Overload", "Overload/Overload.tga");
            case 62:
                return ability(action, "Caution", "Caution/Caution.tga");
            case 63:
                return ability(
                        action,
                        "Exact shot",
                        "ExactShot/ExactShot.tga");
            case 64:
                return ability(
                        action,
                        "Counter fire",
                        "CounterFire/CounterFire.tga");
            case 65:
                return ability(
                        action,
                        "First aid",
                        "FirstAid/FirstAid.tga");
            case 66:
                return ability(action, "Spy mode", "SpyMode/SpyMode.tga");
            case 67:
                return ability(
                        action,
                        "Mobile fortress",
                        "MobileFortress/MobileFortress.tga");
            case 68:
                return ability(
                        action,
                        "Moving fire",
                        "MovingFire/MovingFire.tga");
            case 69:
                return ability(
                        action,
                        "Master of the streets",
                        "MasterOfthestreets/MasterOfthestreets.tga");
            case 70:
                return ability(
                        action,
                        "Adrenaline rush",
                        "AdrenalineRush/AdrenalineRush.tga");
            case 71:
                return ability(
                        action,
                        "Zeroing in",
                        "ZeroingIn/ZeroingIn.tga");
            case 72:
                return ability(action, "Drop bomb", "Bomb/Bomb.tga");
            case 73:
                return ability(
                        action,
                        "Exact bombing",
                        "BombTarget/BombTarget.tga");
            case 74:
                return ability(
                        action,
                        "Smoke shots",
                        "SmokeShots/SmokeShots.tga");
            case 79:
                return ability(
                        action,
                        "Master pilot",
                        "MasterPilot/MasterPilot.tga");
            case 80:
                return ability(
                        action,
                        "Sky guard",
                        "SkyGuard/SkyGuard.tga");
            case 82:
                return ability(
                        action,
                        "Tank hunter",
                        "TankHunter/TankHunter.tga");
            default:
                return null;
        }
    }

    private static Spec fixed(
            int action,
            int slot,
            String label,
            String relativeIconPath) {
        return new Spec(action, slot, false, label, ROOT + relativeIconPath);
    }

    private static Spec ability(
            int action,
            String label,
            String relativeIconPath) {
        return new Spec(action, 0, true, label, ROOT + relativeIconPath);
    }
}
