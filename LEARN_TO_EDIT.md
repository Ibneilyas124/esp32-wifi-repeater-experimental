# LEARN TO EDIT — kahan kya milega

Yeh file sirf tumhare liye hai (compile nahi hoti). Isme har wo text/paragraph
ka pata hai jo repeater ki screens par dikhta hai, aur usay GitHub website se
seedha (edit ✏️ icon se) kaise badalna hai. Poori coding ko haath mat lagana —
sirf neeche batayi gayi **quotes ke andar wali cheez** badalna.

Kisi bhi file ko edit karne ka tareeqa hamesha same hai:
1. GitHub par repo kholo → file pe click karo → pencil ✏️ (Edit this file)
2. Neeche di gayi "dhoondo" wali line Ctrl+F (ya phone par "find in page") se
   dhoondo
3. Quotes ke andar wala text apna likha hua text se replace karo (quotes
   `" "` ko mat chhedna, sirf andar wala hissa)
4. Neeche scroll karke **Commit changes** dabao
5. Naya build khud chal jayega (Actions tab), phir naya firmware flash karna

---

## 1. Login page ka heading — "Sarfraz Ibn E Ilyas" / builder ka naam
**File:** `main/login.html`
**Dhoondo:** `This WiFi Repeater was built by<br>Sarfraz Qureshi`
Yahan naam/tagline badal sakte ho. `<br>` ko mat hataana, yeh sirf line-break hai.

## 2. Galat password ka message
**File:** `main/login.html`
**Dhoondo:** `Wrong password`

## 3. About Developer ka poora popup (contact, skills, quote)
**File:** `main/login.html`
**Dhoondo:** `About Developer` (yeh heading milte hi neeche poora box hai)
Andar yeh sab editable hai:
- Naam: `Sarfraz Qureshi`
- Location: `Naushehro Feroz, Sindh, Pakistan`
- Email: `ahmedsarfraz650@gmail.com` (2 jagah — link aur text dono)
- WhatsApp number: `923043478576` (link ke andar) aur `+92 304 3478576` (dikhne wala text)
- GitHub: `Ibneilyas124` (2 jagah)
- Skills wali list: `<li>...</li>` wali 5 lines
- Neeche wala quote: `Build it. Break it. Fix it. Learn from it.`

## 4. Admin page ka heading — "Sarfraz Ibn E Ilyas — WiFi Repeater Admin"
**File:** `main/admin.html`
**Dhoondo:** `Sarfraz Ibn E Ilyas</h1>` aur thodi neeche `Ibn e Ilyas Technologies`

## 5. Company/business ka naam jo main router mein dikhta hai
Yeh **code mein nahi**, balke ek **setting** hai — dashboard mein khud login
karke Advanced Settings ke andar "Name shown in main router" waali field se
badal sakte ho, reflash ki zaroorat nahi.

## 6. Default AP WiFi naam/password, admin password, DNS, waghera
Yeh bhi sab **settings** hain, code mein nahi — Advanced Settings mein hi
milenge. Agar tumhe sirf **factory-default value** badalni ho (jo naya device
pehli baar boot hone par use karta hai), to:
**File:** `main/main.c`
**Dhoondo:** `static const setting_t SETTINGS[]` — is list ki har line mein
2nd column default value hai (quotes ke andar). Yahan sirf quotes ke andar
wali value badalna, baaki kuch nahi.

## 7. WiFi Tools page ka heading aur tabs (Analyzer/Deauth/Speed/Diagnose)
**File:** `main/tools.html`
**Dhoondo:** `WiFi Tools`, ya tab names jaise `Analyzer`, `Deauth`, `Speed`, `Diagnose`

## 8. Konse buttons/labels kahan hain (quick reference)
| Screen par jo dikhta hai | File | Kya dhoondo |
|---|---|---|
| Login form ka "Login" button | `login.html` | `>Login<` |
| "WiFi Tools (no login)" link | `login.html` | `WiFi Tools` |
| Dashboard ka "Save & Reboot" | `admin.html` | `Save &amp; Reboot` |
| "Factory Reset" button/warning | `admin.html` | `Factory Reset` |
| Hardware reset ka instruction | `admin.html` | `hold the BOOT button` |
| "Share this WiFi" card | `admin.html` | `Share this WiFi` |
| Upstream stability ke reason names | `admin.html` | `var REASONS=` |

---

## Zaroori ehtiyaat
- Sirf **do quotes ke beech** wala matn badalna — quotes, commas, semicolons
  (`;`), curly brackets (`{ }`) waisa hi rehne dena.
- Agar kabhi confuse ho ke yeh C code hai ya HTML text, bas itna yaad rakho:
  `main.c` mein sirf **Section 6** ke `SETTINGS[]` table ke andar ki quotes
  chhedni hain, baaki `main.c` na chhedna.
- Koi bhi edit karne ke baad **Actions tab** mein build zaroor dekhna — agar
  laal ❌ aaye to us error ka pehla `error:` wala hissa Claude ko bhej dena.
