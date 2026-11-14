"""Place names in the spirit of Minecraft's overworld: nature, blocks, items and mobs; no End, no Nether.

A name grows from a root that fits the place (its biome, its height, whether it lies on the coast or what kind of
water it is) and a place word: Oakhollow, Cobble Crag, Sprucewatch, the Mossy Reaches, Kelp Sound, Lake Lilymere.
Every name is unique within one Namer; the same seed and the same requests give the same names.
"""
import numpy as np

ROOTS = {
    'plains': ['Wheat', 'Meadow', 'Hay', 'Pumpkin', 'Carrot', 'Potato', 'Beetroot', 'Cow', 'Sheep', 'Horse', 'Bee',
               'Honey', 'Dandelion', 'Poppy', 'Cornflower', 'Sunflower', 'Oxeye', 'Tulip', 'Villager', 'Rabbit',
               'Cobble', 'Oak', 'Barley', 'Wool', 'Saddle', 'Bread', 'Pig', 'Chicken', 'Lantern', 'Bell'],
    'savannah': ['Acacia', 'Amber', 'Dust', 'Armadillo', 'Llama', 'Coarse', 'Sunbaked', 'Ochre', 'Tallgrass', 'Hay',
                 'Horse', 'Dry', 'Sun', 'Scute', 'Mule', 'Donkey'],
    'mountains': ['Stone', 'Cobble', 'Granite', 'Diorite', 'Andesite', 'Iron', 'Coal', 'Emerald', 'Goat', 'Calcite',
                  'Tuff', 'Gravel', 'Flint', 'Copper', 'Deepslate', 'Amethyst', 'Golem', 'Lapis', 'Redstone',
                  'Diamond', 'Gold', 'Ore', 'Anvil', 'Pickaxe', 'Snow', 'Frost', 'Dripstone', 'Bat', 'Silverfish'],
    'forest': ['Oak', 'Birch', 'Moss', 'Fern', 'Mushroom', 'Wolf', 'Spider', 'Leaf', 'Root', 'Bark', 'Sapling',
               'Azalea', 'Creeper', 'Zombie', 'Bee', 'Acorn', 'Log', 'Lily', 'Fox', 'Owl', 'Pale', 'Hollow', 'Rabbit',
               'Bramble', 'Thorn', 'Skeleton', 'Cobweb'],
    'taiga': ['Spruce', 'Fern', 'Berry', 'Frost', 'Snow', 'Wolf', 'Fox', 'Podzol', 'Ice', 'Bear', 'Stray', 'Rabbit',
              'Cold', 'Sleet', 'Pine', 'Powder', 'Rime', 'Chill', 'Boulder', 'Igloo'],
    'desert': ['Sand', 'Dune', 'Cactus', 'Husk', 'Camel', 'Sandstone', 'Scorch', 'Sun', 'Deadbush', 'Bone', 'Dust',
               'Pyramid', 'Temple', 'Armadillo', 'Terracotta', 'Gold', 'Rabbit', 'Glass', 'Mirage', 'Ember', 'Skull'],
    'jungle': ['Vine', 'Cocoa', 'Bamboo', 'Melon', 'Parrot', 'Ocelot', 'Panda', 'Jungle', 'Fern', 'Moss', 'Leaf',
               'Temple', 'Creeper', 'Emerald', 'Orchid', 'Canopy', 'Liana', 'Cat', 'Spore', 'Bloom'],
    'swamp': ['Mud', 'Mangrove', 'Lily', 'Frog', 'Slime', 'Witch', 'Orchid', 'Reed', 'Cane', 'Bog', 'Moss', 'Mire',
              'Root', 'Drowned', 'Toad', 'Firefly', 'Tadpole', 'Clay', 'Peat', 'Murk', 'Cauldron'],
}
SUFFIXES = {
    'any': ['wood', 'vale', 'hollow', 'ford', 'stead', 'holm', 'crest', 'watch', 'gate', 'field', 'brook', 'ridge',
            'moor', 'dale', 'wick', 'ton', 'bury', 'reach', 'barrow', 'den', 'glen', 'heath', 'march', 'rest', 'run',
            'well', 'hold', 'cross', 'shire', 'stone', 'mill', 'combe', 'ley', 'worth', 'hearth', 'fold'],
    'high': ['crag', 'peak', 'ridge', 'spire', 'fall', 'pass', 'crest', 'cliff', 'rock', 'top', 'scar', 'tor',
             'summit', 'hold', 'height'],
    'wet': ['fen', 'mire', 'bog', 'marsh', 'mere', 'pool', 'wash', 'slough', 'sink', 'reed'],
    'dry': ['dune', 'sands', 'well', 'rest', 'spire', 'flat', 'waste', 'oasis', 'drift', 'basin'],
    'coast': ['haven', 'port', 'bay', 'shore', 'strand', 'cove', 'point', 'head', 'mouth', 'wharf'],
}
LAND_NOUNS = ['Reaches', 'Lands', 'Marches', 'Expanse', 'Wilds', 'Heartland', 'Dominion', 'Frontier', 'Realm',
              'Downs', 'Uplands', 'Lowlands', 'Coast', 'Shires', 'Hinterland']
LAND_ADJECTIVES = {
    'plains': ['Golden', 'Green', 'Rolling', 'Fair', 'Sunny', 'Open'],
    'savannah': ['Amber', 'Dry', 'Sunbaked', 'Tawny', 'Dusty'],
    'mountains': ['Stony', 'High', 'Iron', 'Craggy', 'Grey', 'Lofty'],
    'forest': ['Mossy', 'Green', 'Deep', 'Leafy', 'Shaded', 'Old'],
    'taiga': ['Frozen', 'White', 'Cold', 'Pale', 'Frosty', 'Northern'],
    'desert': ['Burning', 'Golden', 'Endless', 'Scorched', 'Sandy'],
    'jungle': ['Tangled', 'Verdant', 'Wild', 'Lush', 'Humid'],
    'swamp': ['Murky', 'Sunken', 'Misty', 'Drowned', 'Mossy'],
}
REGION_PATTERNS = ['{root} Vale', '{root} Hills', 'The {root} Marches', '{root}shire', '{root} Downs', '{root} Weald',
                   '{root} Valley', 'The {root} Fields', '{root} Hundred', '{root} March']
REGION_HIGH = ['{root} Mountains', 'The {root} Peaks', '{root} Highlands', '{root} Range', 'The {root} Crags']
SEA_ROOTS = ['Kelp', 'Coral', 'Prismarine', 'Squid', 'Turtle', 'Dolphin', 'Salmon', 'Cod', 'Guardian', 'Drowned',
             'Puffer', 'Tide', 'Nautilus', 'Seagrass', 'Sponge', 'Glow Squid', 'Pearl', 'Trident', 'Conduit', 'Wreck',
             'Shell', 'Bubble', 'Riptide', 'Anchor', 'Sail', 'Gull', 'Shipwreck', 'Ink', 'Sea Lantern', 'Tropical']
SEA_COASTAL = ['{root} Bay', '{root} Sound', '{root} Shallows', 'Gulf of {root}', '{root} Strait', '{root} Inlet',
               '{root} Waters', '{root} Reef', '{root} Firth', '{root} Roads']
SEA_OPEN = ['{root} Sea', '{root} Deep', '{root} Ocean', 'The {root} Expanse', '{root} Abyss', '{root} Trench']
LAKE_PATTERNS = ['Lake {root}', '{root}mere', '{root} Lake', '{root} Water']
WASTE_PATTERNS = {
    'mountains': ['The {root} Peaks', '{root} Spires', 'The {adj} Heights'],
    'taiga': ['The {adj} Wastes', 'The {root} Wilds', '{root} Barrens'],
    'desert': ['The {adj} Dunes', 'The {root} Sands', '{root} Barrens'],
    'any': ['The {adj} Wastes', '{root} Barrens', 'The {root} Wilds'],
}


class Namer:
    def __init__(self, seed):
        self.rng = np.random.default_rng(seed)
        self.used = set()

    def pick(self, items):
        return items[int(self.rng.integers(len(items)))]

    def unique(self, make):
        """Call `make` until it gives a name not used yet."""
        for _ in range(2000):
            name = make()
            if name and name not in self.used:
                self.used.add(name)
                return name
        raise RuntimeError('ran out of names')

    def root(self, biome):
        return self.pick(ROOTS.get(biome, ROOTS['plains']))

    def province(self, biome, step, coast):
        """A settlement-like name: one word mostly, two words sometimes."""
        def make():
            kind = 'coast' if coast and self.rng.random() < 0.45 else 'high' if step >= 20 and self.rng.random() < 0.6 \
                else 'wet' if biome == 'swamp' and self.rng.random() < 0.5 \
                else 'dry' if biome in ('desert', 'savannah') and self.rng.random() < 0.35 else 'any'
            root, suffix = self.root(biome), self.pick(SUFFIXES[kind])
            if suffix in root.lower() or root.lower() in suffix:
                return None                       # Hollowhollow, Sandsands
            triple = len(root) > 1 and root[-1].lower() == root[-2].lower() == suffix[0]      # Glassshire
            if ' ' in root or triple or self.rng.random() < 0.25 or root.lower().endswith(suffix[:2]):
                return f'{root} {suffix.capitalize()}'
            return root + suffix
        return self.unique(make)

    def region(self, biome, step):
        patterns = REGION_HIGH if step >= 18 else REGION_PATTERNS
        return self.unique(lambda: self.pick(patterns).format(root=self.root(biome)))

    def land(self, biome):
        adjectives = LAND_ADJECTIVES.get(biome, LAND_ADJECTIVES['plains'])
        return self.unique(lambda: f'The {self.pick(adjectives)} {self.pick(LAND_NOUNS)}')

    def sea(self, open_sea):
        patterns = SEA_OPEN if open_sea else SEA_COASTAL
        return self.unique(lambda: self.pick(patterns).format(root=self.pick(SEA_ROOTS)))

    def lake(self, biome):
        return self.unique(lambda: self.pick(LAKE_PATTERNS).format(root=self.root(biome)))

    def wasteland(self, biome):
        patterns = WASTE_PATTERNS.get(biome, WASTE_PATTERNS['any'])
        adjectives = LAND_ADJECTIVES.get(biome, LAND_ADJECTIVES['plains'])
        return self.unique(lambda: self.pick(patterns).format(root=self.root(biome), adj=self.pick(adjectives)))
