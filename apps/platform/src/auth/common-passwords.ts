// A small bundled list of the most common passwords (lower-case). It is deliberately short: it
// exists to stop the obvious choices (the length rule already rejects anything under 10
// characters, but "password" + digits passes length, so the short bases are listed too).
// checkPassword() tests the password itself, then with trailing digits/punctuation stripped, and
// with common leetspeak undone. For real coverage plug a breached-password check in front of
// validatePassword (documented in docs/api.md, "Passwords").

const LIST = `
password passw0rd p@ssword p@ssw0rd password1 password12 password123 password1234 password12345
pass1234 pass12345 pass123456 passpass passpass123 letmein letmein123 letmein1234 welcome welcome1
welcome123 welcome1234 admin admin123 admin1234 administrator adminadmin root toor rootroot
qwerty qwerty123 qwerty1234 qwerty12345 qwertyuiop qwertyuiop123 qwertyui asdfgh asdfghjkl
asdfghjkl123 asdfasdf zxcvbn zxcvbnm zxcvbnm123 qazwsx qazwsxedc 1qaz2wsx 1q2w3e4r 1q2w3e4r5t
1q2w3e4r5t6y q1w2e3r4 q1w2e3r4t5 qweasdzxc qweqweqwe 123456 1234567 12345678 123456789 1234567890
12345678910 0123456789 0987654321 9876543210 987654321 87654321 11111111 111111111 1111111111
00000000 000000000 0000000000 12341234 123123123 1231231234 123321123 112233445566 1122334455
123454321 abcd1234 abcd12345 abcdefgh abcdefghi abcdefghij abcdefg123 abc123456 abc1234567
iloveyou iloveyou1 iloveyou123 iloveyou12 iloveyou2 ilovegod ilovemyself ilovejesus loveyou
loveme loveme123 lovelove lovelove123 mylove monkey monkey123 monkey1234 dragon dragon123
dragon1234 master master123 mustang shadow shadow123 sunshine sunshine1 sunshine123 princess
princess1 princess123 football football1 football123 baseball baseball1 basketball soccer
superman superman123 batman batman123 spiderman trustno1 trustno1234 freedom hello hello123
hello1234 helloworld helloworld1 hellohello whatever whatever123 michael jordan jordan23 jennifer
jessica ashley daniel thomas charlie charlie123 liverpool arsenal chelsea manchester barcelona
realmadrid ronaldo messi starwars starwars1 pokemon pokemon123 minecraft minecraft1 minecraft123
fortnite fortnite123 gta5 gtav gta5online gta5gta5 grandtheftauto rockstar rockstar1 roleplay
gagemp gage-mp gage1234 gagemp123 gamer gamer123 gaming gaming123 playstation xbox360 nintendo
computer computer1 computer123 internet internet1 cheese cookie cookie123 chocolate banana
secret secret123 secretpass changeme changeme123 changeit default default123 guest guest123
test test123 test1234 testtest testing testing123 temp temp123 temporary user user123 username
login login123 loginlogin access access123 secure secure123 security killer killer123 hunter
hunter2 hunter123 harley ranger summer summer2020 summer2021 summer2022 summer2023 summer2024
summer2025 winter winter2024 winter2025 spring2025 autumn2025 january2025 qwerty2025 password2024
password2025 password2026 welcome2024 welcome2025 welcome2026 iloveu iloveu123 zaq12wsx
zaq1zaq1 zaq1xsw2 1qazxsw2 1qaz1qaz 1qaz2wsx3edc 2wsx3edc pa55word pa$$word pa$$w0rd
`;

export const COMMON_PASSWORDS: ReadonlySet<string> = new Set(
  LIST.split(/\s+/)
    .map((p) => p.trim().toLowerCase())
    .filter((p) => p.length > 0),
);

const LEET: Record<string, string> = { '0': 'o', '1': 'l', '3': 'e', '4': 'a', '5': 's', '7': 't', '@': 'a', $: 's', '!': 'i' };

/** True when the password (any case) is on the list, with or without trailing digits/punctuation or leetspeak. */
export function isCommonPassword(password: string): boolean {
  const lower = password.toLowerCase();
  if (COMMON_PASSWORDS.has(lower)) return true;
  const stripped = lower.replace(/[^a-z]+$/, '');
  if (stripped.length >= 4 && COMMON_PASSWORDS.has(stripped)) return true;
  const unleeted = [...lower].map((ch) => LEET[ch] ?? ch).join('');
  if (unleeted !== lower) {
    if (COMMON_PASSWORDS.has(unleeted)) return true;
    const unleetedStripped = unleeted.replace(/[^a-z]+$/, '');
    if (unleetedStripped.length >= 4 && COMMON_PASSWORDS.has(unleetedStripped)) return true;
  }
  return false;
}
